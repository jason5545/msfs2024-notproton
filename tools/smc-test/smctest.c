/*
 * smctest (steamac r10): self-modifying code patterns under FEX's SMC tracking, as a x86-64 PE for Proton.
 *
 * T1  data writes into a page that also holds code being called, in a loop (what MSFS 2024's four-slot breakpoint phase
 *     does, see logs/r10-log.md): time per iteration. Under SMCChecks=mtrack every write faults and FEX throws away and
 *     recompiles the page's code; with the adaptive hot-page change the page stops being write-protected.
 * T2  code changes on that page: rewrite the imm32 of "mov eax, imm32; ret" and call it, 1000 values. The new value
 *     must be returned every time (the per-instruction validation on hot pages has to catch it).
 * T3  self-modification inside one block: "mov [rip+1], cl; mov eax, imm32; ret" patches the immediate of the next
 *     instruction and must return the patched byte, 1000 values, first on a fresh page (mtrack path, then becoming hot)
 *     and then on the hot page.
 * T4  cross-page store: code on page B patches the imm32 of a function on hot page A, then A is called.
 * T5  no SMC: a plain loop on a separate page that is never written, as a reference for the timing.
 * T6  T1 over 512 pages round-robin (many pages faulting at once, like the game: with a direct-mapped fault counter,
 *     pages that share a counter slot keep resetting each other and never become hot). Reports the time of the second
 *     half, after every page had the chance to become hot, and checks every return value.
 *
 * T7  (added 11/2 for the ARM64EC port) cross-thread code changes on a page that also takes data writes, in lock
 *     step: a writer thread waits until the main thread acknowledged value v-1, then stores imm32 = v into
 *     "mov eax, imm32; ret" (aligned, one store), writes data next to it and publishes v in a sequence word on another
 *     page; the main thread reads the sequence word, calls the function and acks the sequence word. Errors: a result
 *     the writer never stored or ahead of it (more than seq + 1), or a wrong final value after the writer finished.
 *     Calls that still returned an older value than the sequence word are reported (stale_calls) but not counted:
 *     the loop does not serialize before running cross-modified code, which x86 asks for. A third
 *     thread creates and joins short threads all the while, so FEX's thread list changes while code is invalidated.
 * T7u (11/2, information only, not part of RESULT) the same without the lock step: the writer stores 1..N as fast as
 *     it can while the main thread keeps calling. Reports how many calls returned a value older than the sequence word
 *     already read, and the value after the writer finished (should be N). On ARM64EC with SMCChecks=mtrack the final
 *     value stayed stale for good, with CrossOver's own FEX as well; cause not investigated (logs/notproton-log.md,
 *     section on the hot-page port).
 * T8  (11/2) the guest changes the protection of the T7 page (RX, then RWX again) and patches it again: the function
 *     must keep returning the current value.
 *
 * Output goes to smctest-out.txt (Proton starts the program without standard handles). No CRT, no Windows SDK,
 * build with build.sh (Homebrew LLVM).
 */

typedef unsigned long long u64;
typedef unsigned int u32;
typedef unsigned char u8;
typedef long long i64;
typedef void* HANDLE;

__declspec(dllimport) void* __stdcall VirtualAlloc(void* Address, u64 Size, u32 Type, u32 Protect);
__declspec(dllimport) int __stdcall QueryPerformanceCounter(i64* Count);
__declspec(dllimport) int __stdcall QueryPerformanceFrequency(i64* Freq);
__declspec(dllimport) HANDLE __stdcall GetStdHandle(u32 Which);
__declspec(dllimport) int __stdcall WriteFile(HANDLE File, const void* Buffer, u32 Size, u32* Written, void* Overlapped);
__declspec(dllimport) void __stdcall ExitProcess(u32 Code);
__declspec(dllimport) HANDLE __stdcall CreateFileA(const char* Name, u32 Access, u32 Share, void* Security, u32 Disposition, u32 Flags, HANDLE Template);
__declspec(dllimport) int __stdcall CloseHandle(HANDLE Handle);
__declspec(dllimport) HANDLE __stdcall CreateThread(void* Security, u64 StackSize, u32(__stdcall* Start)(void*), void* Param, u32 Flags,
                                                    u32* Tid);
__declspec(dllimport) u32 __stdcall WaitForSingleObject(HANDLE Handle, u32 Ms);
__declspec(dllimport) int __stdcall VirtualProtect(void* Address, u64 Size, u32 NewProt, u32* OldProt);

#define MEM_COMMIT 0x1000u
#define MEM_RESERVE 0x2000u
#define PAGE_READWRITE 0x04u
#define PAGE_EXECUTE_READ 0x20u
#define PAGE_EXECUTE_READWRITE 0x40u
#define INFINITE 0xFFFFFFFFu

void* memset(void* Dest, int Value, unsigned long long Size) {
  volatile unsigned char* D = (volatile unsigned char*)Dest;
  while (Size--) {
    *D++ = (unsigned char)Value;
  }
  return Dest;
}
void* memcpy(void* Dest, const void* Src, unsigned long long Size) {
  volatile unsigned char* D = (volatile unsigned char*)Dest;
  const unsigned char* S = (const unsigned char*)Src;
  while (Size--) {
    *D++ = *S++;
  }
  return Dest;
}

static HANDLE OutFile;
static void puts_(const char* S) {
  u32 Len = 0, Written;
  while (S[Len]) {
    Len++;
  }
  if (OutFile) {
    WriteFile(OutFile, S, Len, &Written, 0);
  }
}
static void dec(u64 V) {
  char Buf[21];
  int i = 20;
  Buf[i] = 0;
  do {
    Buf[--i] = '0' + (V % 10);
    V /= 10;
  } while (V);
  puts_(Buf + i);
}

static i64 Freq;
static i64 Now(void) {
  i64 C;
  QueryPerformanceCounter(&C);
  return C;
}
static u64 Us(i64 Ticks) {
  return (u64)(Ticks * 1000000 / Freq);
}

static void Emit(volatile u8* P, const u8* Code, u32 Len) {
  for (u32 i = 0; i < Len; i++) {
    P[i] = Code[i];
  }
}
static void SetImm32(volatile u8* P, u32 V) {
  P[0] = (u8)V;
  P[1] = (u8)(V >> 8);
  P[2] = (u8)(V >> 16);
  P[3] = (u8)(V >> 24);
}

typedef u32 (*Fn0)(void);
typedef u32 (*Fn1)(u64);

// mov eax, imm32 ; ret
static const u8 MovRet[] = {0xB8, 0x00, 0x00, 0x00, 0x00, 0xC3};
// mov byte [rip+1], cl ; mov eax, imm32 ; ret       (the store patches the low byte of the next imm32)
static const u8 SelfPatch[] = {0x88, 0x0D, 0x01, 0x00, 0x00, 0x00, 0xB8, 0x00, 0x00, 0x00, 0x00, 0xC3};
// T5 reference loop: mov eax, ecx ; add eax, 1 ; ret
static const u8 Plain[] = {0x89, 0xC8, 0x83, 0xC0, 0x01, 0xC3};

static u32 T3(const char* Name, volatile u8* Page, u32 Offset) {
  Emit(Page + Offset, SelfPatch, sizeof(SelfPatch));
  Fn1 F = (Fn1)(Page + Offset);
  u32 Errors = 0;
  i64 T0 = Now();
  for (u32 i = 1; i <= 1000; i++) {
    u32 Want = i & 0xFF;
    u32 Got = F(Want);
    if (Got != Want) {
      if (Errors < 5) {
        puts_("  T3 mismatch want ");
        dec(Want);
        puts_(" got ");
        dec(Got);
        puts_("\n");
      }
      Errors++;
    }
  }
  i64 T1 = Now();
  puts_(Name);
  puts_(" errors ");
  dec(Errors);
  puts_(" time_us ");
  dec(Us(T1 - T0));
  puts_("\n");
  return Errors;
}

// T7 shared state. Code page: "nop; nop; nop; mov eax, imm32; ret" with the imm32 4-byte aligned at +4, data at +0x800.
// Seq, Ack, Done and the thread counter live on a separate read-write (non-executable) page.
static volatile u8* T7Code;
static volatile u32* T7Seq;
static volatile u32* T7Ack;
static volatile u32* T7Done;
static volatile u32* T7Spawned;
static u32 T7N;
static u32 T7LockStep;

static u32 __stdcall T7Writer(void* Unused) {
  volatile u32* Imm = (volatile u32*)(T7Code + 4);
  volatile u64* Data = (volatile u64*)(T7Code + 0x800);
  for (u32 v = 1; v <= T7N; v++) {
    if (T7LockStep) {
      while (*T7Ack != v - 1) {
        __builtin_ia32_pause();
      }
    }
    *Imm = v;
    *Data = v;
    *T7Seq = v;
  }
  *T7Done = 1;
  return 0;
}

// Runs one T7 pass on page D (code already emitted there) with sequence words on page S. Returns the error count.
static u32 T7Run(volatile u8* D, volatile u8* S, u32 N, u32 LockStep, const char* Name);

static u32 __stdcall T7Nop(void* Unused) {
  return 0;
}

static u32 __stdcall T7Spawner(void* Unused) {
  while (!*T7Done) {
    HANDLE H = CreateThread(0, 0, T7Nop, 0, 0, 0);
    if (H) {
      WaitForSingleObject(H, INFINITE);
      CloseHandle(H);
      (*T7Spawned)++;
    }
  }
  return 0;
}

static u32 T7Run(volatile u8* D, volatile u8* S, u32 N, u32 LockStep, const char* Name) {
  static const u8 Code[] = {0x90, 0x90, 0x90, 0xB8, 0x00, 0x00, 0x00, 0x00, 0xC3};
  Emit(D, Code, sizeof(Code));
  T7Code = D;
  T7Seq = (volatile u32*)S;
  T7Ack = (volatile u32*)(S + 32);
  T7Done = (volatile u32*)(S + 64);
  T7Spawned = (volatile u32*)(S + 128);
  *T7Seq = 0;
  *T7Ack = 0;
  *T7Done = 0;
  *T7Spawned = 0;
  T7N = N;
  T7LockStep = LockStep;
  Fn0 F = (Fn0)D;
  F(); // compile it once before the writer starts
  HANDLE Spawner = CreateThread(0, 0, T7Spawner, 0, 0, 0);
  HANDLE Writer = CreateThread(0, 0, T7Writer, 0, 0, 0);
  u64 Calls = 0, Bad = 0, Range = 0, Advanced = 0;
  u32 Last = 0;
  i64 T0 = Now();
  for (;;) {
    const u32 Done = *T7Done;
    const u32 Seen = *T7Seq;
    const u32 Got = F();
    Calls++;
    // Older than Seen: the store of the new code is already visible as data, the call still ran the old code. Counted
    // as information only: x86 asks the executing processor to serialize before it runs cross-modified code, and this
    // loop does not. Lock step: the writer stores v + 1 only after we acked v and may already have stored the code (not
    // yet the sequence word) for Seen + 1, so more than Seen + 1 is wrong.
    if (Got < Seen) {
      Bad++;
    }
    if (LockStep && Got > Seen + 1) {
      if (Bad < 5) {
        puts_("  ");
        puts_(Name);
        puts_(": seq ");
        dec(Seen);
        puts_(" got ");
        dec(Got);
        puts_("\n");
      }
      Range++;
    }
    if (LockStep) {
      *T7Ack = Seen;
    }
    if (Got > N) {
      Range++;
    }
    if (Got > Last) {
      Advanced++;
      Last = Got;
    }
    if (Done) {
      break;
    }
  }
  i64 T1 = Now();
  WaitForSingleObject(Writer, INFINITE);
  WaitForSingleObject(Spawner, INFINITE);
  CloseHandle(Writer);
  CloseHandle(Spawner);
  const u32 Final = F();
  // Errors: a value the writer never stored or (lock step) ahead of it, or the final value after the writer finished.
  const u32 E = (u32)Range + (Final != N);
  puts_(Name);
  puts_(" n ");
  dec(N);
  puts_(" calls ");
  dec(Calls);
  puts_(" distinct_values_seen ");
  dec(Advanced);
  puts_(" stale_calls ");
  dec(Bad);
  puts_(" out_of_range ");
  dec(Range);
  puts_(" final ");
  dec(Final);
  puts_(" threads_spawned ");
  dec(*T7Spawned);
  puts_(" time_us ");
  dec(Us(T1 - T0));
  puts_(" errors ");
  dec(E);
  puts_("\n");
  return E;
}

void __stdcall Entry(void) {
  OutFile = CreateFileA("smctest-out.txt", 0x40000000 /* GENERIC_WRITE */, 1, 0, 2 /* CREATE_ALWAYS */, 0x80, 0);
  QueryPerformanceFrequency(&Freq);
  u32 Errors = 0;

  // Page A: function at 0x000, self-patching function at 0x100, data at 0x800. Page B: fresh page for T3 and T4.
  // Page C: T5 reference code, never written after setup.
  volatile u8* A = (volatile u8*)VirtualAlloc(0, 3 * 4096, MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE);
  volatile u8* B = A + 4096;
  volatile u8* C = A + 8192;
  if (!A) {
    puts_("VirtualAlloc failed\nRESULT FAIL\n");
    ExitProcess(1);
  }
  puts_("smctest: pages ");
  dec((u64)A);
  puts_("\n");

  Emit(A, MovRet, sizeof(MovRet));
  SetImm32(A + 1, 42);
  Emit(C, Plain, sizeof(Plain));
  Fn0 F = (Fn0)(A);
  Fn1 G = (Fn1)(C);
  volatile u64* Data = (volatile u64*)(A + 0x800);

  // T5 first, before page A gets any writes.
  {
    u64 Sum = 0;
    i64 T0 = Now();
    for (u32 i = 0; i < 20000; i++) {
      Sum += G(i);
    }
    i64 T1 = Now();
    puts_("T5 plain loop 20000 iterations time_us ");
    dec(Us(T1 - T0));
    puts_(" sum ");
    dec(Sum);
    puts_("\n");
  }

  // T1: data write into the code page + call, 20000 iterations.
  {
    u64 Sum = 0;
    i64 T0 = Now();
    for (u32 i = 0; i < 20000; i++) {
      *Data = i;
      Sum += F();
    }
    i64 T1 = Now();
    u64 T = Us(T1 - T0);
    puts_("T1 data-in-code-page loop 20000 iterations time_us ");
    dec(T);
    puts_(" ns_per_iter ");
    dec(T * 1000 / 20000);
    puts_(" sum ");
    dec(Sum);
    puts_(Sum == 42ull * 20000 ? " ok\n" : " WRONG\n");
    if (Sum != 42ull * 20000) {
      Errors++;
    }
  }

  // T2: change the code on page A and call it.
  {
    u32 E = 0;
    i64 T0 = Now();
    for (u32 v = 0; v < 1000; v++) {
      u32 Want = v * 7 + 3;
      SetImm32(A + 1, Want);
      *Data = v;
      u32 Got = F();
      if (Got != Want) {
        if (E < 5) {
          puts_("  T2 mismatch want ");
          dec(Want);
          puts_(" got ");
          dec(Got);
          puts_("\n");
        }
        E++;
      }
    }
    i64 T1 = Now();
    puts_("T2 code change on data page errors ");
    dec(E);
    puts_(" time_us ");
    dec(Us(T1 - T0));
    puts_("\n");
    Errors += E;
  }

  // T3: self-modification inside one block, on a fresh page (B), then on page A.
  Errors += T3("T3 self-patch fresh page", B, 0);
  Errors += T3("T3 self-patch data page", A, 0x100);

  // T4: code on page B patches the function on page A (mov dword [rip+disp32], ecx ; ret), then A is called.
  {
    // 89 0D disp32 = mov [rip+disp32], ecx ; target = A + 1 (imm32 of MovRet), next ip = B + 0x206.
    volatile u8* P = B + 0x200;
    i64 Disp = (i64)((A + 1) - (P + 6));
    u8 Code[] = {0x89, 0x0D, (u8)Disp, (u8)(Disp >> 8), (u8)(Disp >> 16), (u8)(Disp >> 24), 0xC3};
    Emit(P, Code, sizeof(Code));
    Fn1 Patch = (Fn1)P;
    u32 E = 0;
    for (u32 v = 0; v < 1000; v++) {
      u32 Want = 0x10000 + v * 13;
      Patch(Want);
      u32 Got = F();
      if (Got != Want) {
        if (E < 5) {
          puts_("  T4 mismatch want ");
          dec(Want);
          puts_(" got ");
          dec(Got);
          puts_("\n");
        }
        E++;
      }
    }
    puts_("T4 cross-page patch errors ");
    dec(E);
    puts_("\n");
    Errors += E;
  }

  // T6: 512 pages, each "mov eax, imm32 ; ret" at 0 (imm32 = page index) and a data slot at 0x800.
  {
    enum { NPages = 512, Rounds = 40 };
    volatile u8* M = (volatile u8*)VirtualAlloc(0, (u64)NPages * 4096, MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE);
    if (!M) {
      puts_("T6 VirtualAlloc failed\n");
      Errors++;
    } else {
      for (u32 p = 0; p < NPages; p++) {
        Emit(M + (u64)p * 4096, MovRet, sizeof(MovRet));
        SetImm32(M + (u64)p * 4096 + 1, p);
      }
      u32 E = 0;
      i64 Half = 0, T0 = Now();
      for (u32 r = 0; r < Rounds; r++) {
        if (r == Rounds / 2) {
          Half = Now();
        }
        for (u32 p = 0; p < NPages; p++) {
          volatile u8* Pg = M + (u64)p * 4096;
          *(volatile u64*)(Pg + 0x800) = r;
          if (((Fn0)Pg)() != p) {
            E++;
          }
        }
      }
      i64 T1 = Now();
      u64 Second = Us(T1 - Half);
      puts_("T6 512 pages x 40 rounds errors ");
      dec(E);
      puts_(" total_us ");
      dec(Us(T1 - T0));
      puts_(" second_half_us ");
      dec(Second);
      puts_(" ns_per_call_second_half ");
      dec(Second * 1000 / (NPages * (Rounds / 2)));
      puts_("\n");
      Errors += E;
    }
  }

  // T7 (lock step, counted), T7u (free running, information only), T8 (protection changes on the T7 page).
  {
    volatile u8* D = (volatile u8*)VirtualAlloc(0, 4096, MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE);
    volatile u8* DU = (volatile u8*)VirtualAlloc(0, 4096, MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE);
    volatile u8* S = (volatile u8*)VirtualAlloc(0, 4096, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
    if (!D || !DU || !S) {
      puts_("T7 VirtualAlloc failed\n");
      Errors++;
    } else {
      Errors += T7Run(D, S, 5000, 1, "T7 lock-step cross-thread patch");
      T7Run(DU, S + 1024, 20000, 0, "T7u free-running cross-thread patch (info, not counted)");

      Fn0 F7 = (Fn0)D;
      u32 Old = 0, E8 = 0;
      volatile u32* Imm = (volatile u32*)(D + 4);
      if (!VirtualProtect((void*)D, 4096, PAGE_EXECUTE_READ, &Old)) {
        puts_("  T8 VirtualProtect RX failed\n");
        E8++;
      }
      E8 += F7() != 5000;
      if (!VirtualProtect((void*)D, 4096, PAGE_EXECUTE_READWRITE, &Old)) {
        puts_("  T8 VirtualProtect RWX failed\n");
        E8++;
      }
      for (u32 v = 0; v < 1000; v++) {
        *(volatile u64*)(D + 0x800) = v;
        *Imm = 0x50000 + v;
        E8 += F7() != 0x50000 + v;
      }
      puts_("T8 protection change errors ");
      dec(E8);
      puts_("\n");
      Errors += E8;
    }
  }

  puts_(Errors ? "RESULT FAIL\n" : "RESULT PASS\n");
  CloseHandle(OutFile);
  ExitProcess(Errors ? 1 : 0);
}
