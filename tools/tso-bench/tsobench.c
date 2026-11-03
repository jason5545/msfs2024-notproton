/*
 * tsobench: why the MSFS 2024 "four-slot" phase is ~4x slower on NotProton than on steamac.
 *
 * steamac ran FEX in a Linux VM with hardware TSO (PR_SET_MEM_MODEL=TSO). Our ARM64EC FEX on NotProton has no unix
 * library, so FEX emulates x86 memory ordering in software (ordered loads/stores on every non-stack access). This
 * program measures the pieces separately, so the same .exe can be compared across FEX builds and settings:
 *
 *   MP    message-passing litmus test (writer: data then flag; reader: flag then data). x86 never shows a reader that
 *         sees the new flag with old data. Violations > 0 means the run has no x86 memory ordering at all (e.g.
 *         FEX_TSOENABLED=0); 0 means software or hardware TSO is in effect.
 *   MEM   single-thread loads/stores on heap memory (struct array, dependent index), with a second thread alive,
 *         ns per iteration. This is where software TSO costs.
 *   COPY  dst[i] = src[i] + p over 16 KiB, independent loads and stores, ns per word.
 *   SUM   four independent load streams over 16 KiB, ns per word.
 *   LOCK  lock xadd on heap memory, ns per op (same instructions in every mode, baseline).
 *   CALL  call a small function, ns per call (baseline for DB).
 *   DB    execute breakpoint in DR0 + vectored handler returning CONTINUE_EXECUTION, ns per hit (what the game's
 *         four-slot and main loop do on every round/frame). Needs our FEX with the debug register patch; on other
 *         FEX builds it reports 0 hits.
 *
 * No CRT, no Windows SDK. See build.sh. Output also goes to tsobench-out.txt in the current directory.
 */

typedef unsigned long long u64;
typedef unsigned int u32;
typedef unsigned short u16;
typedef long LONG;
typedef void* HANDLE;

typedef struct __attribute__((aligned(16))) {
  u64 Low, High;
} M128A;

typedef struct __attribute__((aligned(16))) CONTEXT {
  u64 P1Home, P2Home, P3Home, P4Home, P5Home, P6Home;
  u32 ContextFlags;
  u32 MxCsr;
  u16 SegCs, SegDs, SegEs, SegFs, SegGs, SegSs;
  u32 EFlags;
  u64 Dr0, Dr1, Dr2, Dr3, Dr6, Dr7;
  u64 Rax, Rcx, Rdx, Rbx, Rsp, Rbp, Rsi, Rdi, R8, R9, R10, R11, R12, R13, R14, R15;
  u64 Rip;
  unsigned char FltSave[512];
  M128A VectorRegister[26];
  u64 VectorControl, DebugControl, LastBranchToRip, LastBranchFromRip, LastExceptionToRip, LastExceptionFromRip;
} CONTEXT;
_Static_assert(sizeof(CONTEXT) == 0x4d0, "CONTEXT size");

typedef struct EXCEPTION_RECORD {
  u32 ExceptionCode;
  u32 ExceptionFlags;
  struct EXCEPTION_RECORD* ExceptionRecord;
  void* ExceptionAddress;
  u32 NumberParameters;
  u64 ExceptionInformation[15];
} EXCEPTION_RECORD;

typedef struct {
  EXCEPTION_RECORD* ExceptionRecord;
  CONTEXT* ContextRecord;
} EXCEPTION_POINTERS;

#define CONTEXT_AMD64 0x00100000u
#define CONTEXT_DEBUG_REGISTERS (CONTEXT_AMD64 | 0x10u)
#define EXCEPTION_SINGLE_STEP 0x80000004u
#define EXCEPTION_CONTINUE_EXECUTION (-1)
#define EXCEPTION_CONTINUE_SEARCH 0
#define STD_OUTPUT_HANDLE ((u32)-11)
#define MEM_COMMIT 0x1000u
#define MEM_RESERVE 0x2000u
#define PAGE_READWRITE 0x04u

__declspec(dllimport) void* __stdcall AddVectoredExceptionHandler(u32 First, LONG(__stdcall* Handler)(EXCEPTION_POINTERS*));
__declspec(dllimport) HANDLE __stdcall GetCurrentThread(void);
__declspec(dllimport) int __stdcall SetThreadContext(HANDLE Thread, const CONTEXT* Context);
__declspec(dllimport) HANDLE __stdcall GetStdHandle(u32 Which);
__declspec(dllimport) int __stdcall WriteFile(HANDLE File, const void* Buffer, u32 Size, u32* Written, void* Overlapped);
__declspec(dllimport) void __stdcall ExitProcess(u32 Code);
__declspec(dllimport) HANDLE __stdcall CreateThread(void* Attributes, u64 StackSize, u32(__stdcall* Start)(void*), void* Param, u32 Flags, u32* Id);
__declspec(dllimport) u32 __stdcall WaitForSingleObject(HANDLE Handle, u32 Milliseconds);
__declspec(dllimport) void __stdcall Sleep(u32 Milliseconds);
__declspec(dllimport) HANDLE __stdcall CreateFileA(const char* Name, u32 Access, u32 Share, void* Security, u32 Disposition, u32 Flags, HANDLE Template);
__declspec(dllimport) int __stdcall CloseHandle(HANDLE Handle);
__declspec(dllimport) void* __stdcall VirtualAlloc(void* Address, u64 Size, u32 Type, u32 Protect);
__declspec(dllimport) int __stdcall QueryPerformanceCounter(u64* Count);
__declspec(dllimport) int __stdcall QueryPerformanceFrequency(u64* Freq);

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

static HANDLE Out, OutFile;
static void puts_(const char* S) {
  u32 Len = 0, Written;
  while (S[Len]) {
    Len++;
  }
  if (Out) {
    WriteFile(Out, S, Len, &Written, 0);
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

static u64 Freq;
static u64 Now(void) {
  u64 C;
  QueryPerformanceCounter(&C);
  return C;
}
static u64 ElapsedNs(u64 Start, u64 End) {
  return (End - Start) * 1000000000ull / Freq;
}
// "<name> <iterations> iterations <total ms> ms <ps per iteration / 1000 with 3 decimals> ns/iter"
static void Report(const char* Name, u64 Iterations, u64 Ns) {
  puts_(Name);
  puts_(" n=");
  dec(Iterations);
  puts_(" ms=");
  dec(Ns / 1000000);
  puts_(" ns/iter=");
  const u64 Ps = Ns * 1000 / Iterations;
  dec(Ps / 1000);
  puts_(".");
  const u64 Frac = Ps % 1000;
  puts_(Frac < 100 ? (Frac < 10 ? "00" : "0") : "");
  dec(Frac);
  puts_("\n");
}

// ---- MP litmus ----
typedef struct {
  volatile u64 Data;
  char Pad1[56];
  volatile u64 Flag;
  char Pad2[56];
  volatile u32 Start;
  volatile u32 Done;
} MPShared;
static MPShared* MP;
#define MP_ROUNDS 20000000ull

static u32 __stdcall MPWriter(void* Param) {
  (void)Param;
  while (!MP->Start) {
  }
  for (u64 i = 1; i <= MP_ROUNDS; i++) {
    MP->Data = i;
    MP->Flag = i;
  }
  MP->Done = 1;
  return 0;
}

// ---- MEM ----
typedef struct {
  u64 A, B, C, D;
} Item;
#define MEM_ITEMS 4096u // 128 KiB, stays in cache: measures instruction cost, not memory bandwidth
#define MEM_ITERS 50000000ull

__attribute__((noinline)) static u64 MemLoop(Item* Items, u64 Iters) {
  u64 Idx = 1, Acc = 0;
  for (u64 i = 0; i < Iters; i++) {
    Item* It = &Items[Idx & (MEM_ITEMS - 1)];
    const u64 V = It->A + It->B;
    It->C = V;
    It->D += i;
    Acc += It->C ^ It->D;
    Idx = Idx * 5 + V + 1;
  }
  return Acc;
}

// Independent loads/stores (throughput, not a dependent chain): copy with +1, and a load-only sum.
#define ARR_WORDS 2048u // 16 KiB each
#define ARR_PASSES 20000ull
__attribute__((noinline)) static void CopyLoop(volatile u64* Dst, const volatile u64* Src, u64 Passes) {
  for (u64 p = 0; p < Passes; p++) {
    for (u32 i = 0; i < ARR_WORDS; i++) {
      Dst[i] = Src[i] + p;
    }
  }
}
__attribute__((noinline)) static u64 SumLoop(const volatile u64* Src, u64 Passes) {
  u64 S0 = 0, S1 = 0, S2 = 0, S3 = 0;
  for (u64 p = 0; p < Passes; p++) {
    for (u32 i = 0; i < ARR_WORDS; i += 4) {
      S0 += Src[i];
      S1 += Src[i + 1];
      S2 += Src[i + 2];
      S3 += Src[i + 3];
    }
  }
  return S0 + S1 + S2 + S3;
}

// ---- LOCK ----
#define LOCK_ITERS 20000000ull
__attribute__((noinline)) static void LockLoop(volatile u64* Counter, u64 Iters) {
  for (u64 i = 0; i < Iters; i++) {
    __atomic_fetch_add(Counter, 1, __ATOMIC_SEQ_CST);
  }
}

// ---- CALL / DB ----
__attribute__((noinline)) int Target(int X) {
  __asm__ volatile("" ::: "memory");
  return X * 3 + 1;
}
static volatile u64 Hits;
static LONG __stdcall Handler(EXCEPTION_POINTERS* Info) {
  if (Info->ExceptionRecord->ExceptionCode == EXCEPTION_SINGLE_STEP) {
    Hits++;
    return EXCEPTION_CONTINUE_EXECUTION;
  }
  return EXCEPTION_CONTINUE_SEARCH;
}
static int SetDR0(u64 Dr0, u64 Dr7) {
  CONTEXT Ctx;
  memset(&Ctx, 0, sizeof(Ctx));
  Ctx.ContextFlags = CONTEXT_DEBUG_REGISTERS;
  Ctx.Dr0 = Dr0;
  Ctx.Dr7 = Dr7;
  return SetThreadContext(GetCurrentThread(), &Ctx);
}

static volatile u32 IdleStop;
static u32 __stdcall Idle(void* Param) {
  (void)Param;
  while (!IdleStop) {
    Sleep(10);
  }
  return 0;
}

void __stdcall Entry(void) {
  Out = GetStdHandle(STD_OUTPUT_HANDLE);
  OutFile = CreateFileA("tsobench-out.txt", 0x40000000, 1, 0, 2, 0x80, 0);
  if (OutFile == (HANDLE)-1) {
    OutFile = 0;
  }
  QueryPerformanceFrequency(&Freq);
  puts_("tsobench\n");

  // A second thread for the whole run: FEX may treat a single-threaded process differently.
  HANDLE IdleThread = CreateThread(0, 0, Idle, 0, 0, 0);

  // MP
  MP = (MPShared*)VirtualAlloc(0, 4096, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
  HANDLE W = CreateThread(0, 0, MPWriter, 0, 0, 0);
  u64 Violations = 0, Reads = 0;
  u64 T0 = Now();
  MP->Start = 1;
  while (!MP->Done) {
    const u64 F = MP->Flag;
    const u64 D = MP->Data;
    Violations += D < F;
    Reads++;
  }
  u64 T1 = Now();
  WaitForSingleObject(W, 0xFFFFFFFFu);
  puts_("MP reads=");
  dec(Reads);
  puts_(" violations=");
  dec(Violations);
  puts_(" ms=");
  dec(ElapsedNs(T0, T1) / 1000000);
  puts_("\n");

  // MEM (3 runs)
  Item* Items = (Item*)VirtualAlloc(0, sizeof(Item) * MEM_ITEMS, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
  for (u32 i = 0; i < MEM_ITEMS; i++) {
    Items[i].A = i * 3;
    Items[i].B = i ^ 0x55;
  }
  volatile u64 Sink = 0;
  for (int r = 0; r < 3; r++) {
    T0 = Now();
    Sink += MemLoop(Items, MEM_ITERS);
    T1 = Now();
    Report("MEM", MEM_ITERS, ElapsedNs(T0, T1));
  }

  // COPY / SUM
  volatile u64* ArrA = (volatile u64*)VirtualAlloc(0, ARR_WORDS * 8, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
  volatile u64* ArrB = (volatile u64*)VirtualAlloc(0, ARR_WORDS * 8, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
  for (int r = 0; r < 2; r++) {
    T0 = Now();
    CopyLoop(ArrB, ArrA, ARR_PASSES);
    T1 = Now();
    Report("COPY (per word)", ARR_PASSES * ARR_WORDS, ElapsedNs(T0, T1));
  }
  for (int r = 0; r < 2; r++) {
    T0 = Now();
    Sink += SumLoop(ArrB, ARR_PASSES);
    T1 = Now();
    Report("SUM (per word)", ARR_PASSES * ARR_WORDS, ElapsedNs(T0, T1));
  }

  // LOCK
  volatile u64* Counter = (volatile u64*)VirtualAlloc(0, 4096, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
  T0 = Now();
  LockLoop(Counter, LOCK_ITERS);
  T1 = Now();
  Report("LOCK", LOCK_ITERS, ElapsedNs(T0, T1));

  // CALL
  volatile int Sum = 0;
  const u64 CallIters = 20000000ull;
  T0 = Now();
  for (u64 i = 0; i < CallIters; i++) {
    Sum += Target((int)i);
  }
  T1 = Now();
  Report("CALL", CallIters, ElapsedNs(T0, T1));

  // DB
  AddVectoredExceptionHandler(1, Handler);
  const int Ok = SetDR0((u64)&Target, 0x1);
  const u64 DbIters = 20000;
  Sum += Target(1); // warm up
  const u64 HitsBefore = Hits;
  T0 = Now();
  for (u64 i = 0; i < DbIters; i++) {
    Sum += Target((int)i);
  }
  T1 = Now();
  SetDR0(0, 0);
  puts_("DB set=");
  dec(Ok);
  puts_(" hits=");
  dec(Hits - HitsBefore);
  puts_(" ");
  Report("", DbIters, ElapsedNs(T0, T1));

  IdleStop = 1;
  WaitForSingleObject(IdleThread, 1000);
  puts_("done\n");
  if (OutFile) {
    CloseHandle(OutFile);
  }
  ExitProcess(0);
}
