/*
 * drtest: x86 hardware execute breakpoint on the calling thread, the way MSFS 2024 uses it at boot.
 *
 * Phase A (what the game does): SetThreadContext(GetCurrentThread()) with DR0 = an address in this program and
 * DR7 = 0x155, a vectored exception handler that takes EXCEPTION_SINGLE_STEP and returns
 * EXCEPTION_CONTINUE_EXECUTION, then call the function three times. On x86 Windows / x86 Linux + Wine every call raises
 * one SINGLE_STEP at the function's first instruction, with Dr6 bit 0 set and EFLAGS.RF set. Then clear DR7 and check
 * that the breakpoint is gone.
 *
 * Phase B (extra, not used by the game): the same through SuspendThread / SetThreadContext / ResumeThread on another
 * thread.
 *
 * No CRT, no Windows SDK: the few types and imports needed are declared here. See build.sh.
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
_Static_assert(__builtin_offsetof(CONTEXT, Dr0) == 0x48, "Dr0 offset");
_Static_assert(__builtin_offsetof(CONTEXT, Dr6) == 0x68, "Dr6 offset");
_Static_assert(__builtin_offsetof(CONTEXT, Rip) == 0xf8, "Rip offset");

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
#define INFINITE 0xFFFFFFFFu

__declspec(dllimport) void* __stdcall AddVectoredExceptionHandler(u32 First, LONG(__stdcall* Handler)(EXCEPTION_POINTERS*));
__declspec(dllimport) HANDLE __stdcall GetCurrentThread(void);
__declspec(dllimport) u32 __stdcall GetCurrentThreadId(void);
__declspec(dllimport) int __stdcall SetThreadContext(HANDLE Thread, const CONTEXT* Context);
__declspec(dllimport) int __stdcall GetThreadContext(HANDLE Thread, CONTEXT* Context);
__declspec(dllimport) u32 __stdcall GetLastError(void);
__declspec(dllimport) HANDLE __stdcall GetStdHandle(u32 Which);
__declspec(dllimport) int __stdcall WriteFile(HANDLE File, const void* Buffer, u32 Size, u32* Written, void* Overlapped);
__declspec(dllimport) void __stdcall ExitProcess(u32 Code);
__declspec(dllimport) HANDLE __stdcall CreateThread(void* Attributes, u64 StackSize, u32(__stdcall* Start)(void*), void* Param, u32 Flags, u32* Id);
__declspec(dllimport) u32 __stdcall WaitForSingleObject(HANDLE Handle, u32 Milliseconds);
__declspec(dllimport) u32 __stdcall SuspendThread(HANDLE Thread);
__declspec(dllimport) u32 __stdcall ResumeThread(HANDLE Thread);
__declspec(dllimport) void __stdcall Sleep(u32 Milliseconds);
__declspec(dllimport) HANDLE __stdcall CreateFileA(const char* Name, u32 Access, u32 Share, void* Security, u32 Disposition, u32 Flags, HANDLE Template);
__declspec(dllimport) int __stdcall CloseHandle(HANDLE Handle);

// The compiler may emit calls to these for struct initialization.
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

// Proton starts the program without standard handles, so the output also goes to drtest-out.txt in the current
// directory.
static HANDLE Out;
static HANDLE OutFile;
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
static void hex(u64 V) {
  char Buf[19];
  Buf[0] = '0';
  Buf[1] = 'x';
  for (int i = 0; i < 16; i++) {
    Buf[2 + i] = "0123456789abcdef"[(V >> ((15 - i) * 4)) & 0xf];
  }
  Buf[18] = 0;
  puts_(Buf);
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

// Breakpoint targets. noinline + used so each has its own first instruction.
__attribute__((noinline)) int TargetA(int X) {
  __asm__ volatile("" ::: "memory");
  return X * 3 + 1;
}
__attribute__((noinline)) int TargetB(int X) {
  __asm__ volatile("" ::: "memory");
  return X * 5 + 2;
}

static volatile u32 Hits;
static volatile u64 LastAddress;
static volatile u64 LastDr6;
static volatile u32 LastEFlags;
static volatile u32 LastThread;
static volatile u32 OtherCode;

static LONG __stdcall Handler(EXCEPTION_POINTERS* Info) {
  if (Info->ExceptionRecord->ExceptionCode == EXCEPTION_SINGLE_STEP) {
    Hits++;
    LastAddress = (u64)Info->ExceptionRecord->ExceptionAddress;
    LastDr6 = Info->ContextRecord->Dr6;
    LastEFlags = Info->ContextRecord->EFlags;
    LastThread = GetCurrentThreadId();
    return EXCEPTION_CONTINUE_EXECUTION;
  }
  OtherCode = Info->ExceptionRecord->ExceptionCode;
  return EXCEPTION_CONTINUE_SEARCH;
}

static int SetDR(HANDLE Thread, u64 Dr0, u64 Dr7) {
  CONTEXT Ctx;
  memset(&Ctx, 0, sizeof(Ctx));
  Ctx.ContextFlags = CONTEXT_DEBUG_REGISTERS;
  Ctx.Dr0 = Dr0;
  Ctx.Dr7 = Dr7;
  int Ok = SetThreadContext(Thread, &Ctx);
  puts_("  SetThreadContext(Dr0=");
  hex(Dr0);
  puts_(", Dr7=");
  hex(Dr7);
  puts_(") = ");
  dec(Ok);
  if (!Ok) {
    puts_(" GetLastError=");
    dec(GetLastError());
  }
  puts_("\n");
  return Ok;
}

static void ShowDR(HANDLE Thread) {
  CONTEXT Ctx;
  memset(&Ctx, 0, sizeof(Ctx));
  Ctx.ContextFlags = CONTEXT_DEBUG_REGISTERS;
  int Ok = GetThreadContext(Thread, &Ctx);
  puts_("  GetThreadContext = ");
  dec(Ok);
  puts_(" Dr0=");
  hex(Ctx.Dr0);
  puts_(" Dr6=");
  hex(Ctx.Dr6);
  puts_(" Dr7=");
  hex(Ctx.Dr7);
  puts_("\n");
}

static void ShowHit(void) {
  puts_("  hits=");
  dec(Hits);
  puts_(" addr=");
  hex(LastAddress);
  puts_(" Dr6=");
  hex(LastDr6);
  puts_(" EFlags=");
  hex(LastEFlags);
  puts_(" tid=");
  dec(LastThread);
  puts_("\n");
}

static volatile u32 Go;
static volatile int WorkerResult;
static u32 __stdcall Worker(void* Param) {
  (void)Param;
  while (!Go) {
    Sleep(1);
  }
  WorkerResult = TargetB(7);
  return 0;
}

void __stdcall Entry(void) {
  Out = GetStdHandle(STD_OUTPUT_HANDLE);
  OutFile = CreateFileA("drtest-out.txt", 0x40000000 /* GENERIC_WRITE */, 1 /* FILE_SHARE_READ */, 0, 2 /* CREATE_ALWAYS */, 0x80, 0);
  if (OutFile == (HANDLE)-1) {
    OutFile = 0;
  }
  int Fail = 0;
  volatile int Sum = 0;

  puts_("drtest: TargetA=");
  hex((u64)&TargetA);
  puts_(" TargetB=");
  hex((u64)&TargetB);
  puts_(" main tid=");
  dec(GetCurrentThreadId());
  puts_("\n");
  AddVectoredExceptionHandler(1, Handler);

  puts_("phase A: DR0 execute breakpoint on the calling thread (like MSFS 2024)\n");
  int Ok = SetDR(GetCurrentThread(), (u64)&TargetA, 0x155);
  ShowDR(GetCurrentThread());
  for (int i = 1; i <= 3; i++) {
    Sum += TargetA(i);
    puts_("  after call ");
    dec(i);
    puts_(":");
    ShowHit();
  }
  const int ResultsOk = Sum == (4 + 7 + 10);
  const int A = Ok && Hits == 3 && LastAddress == (u64)&TargetA && (LastDr6 & 0xf) == 1 && (LastEFlags & 0x10000) && ResultsOk;
  ShowDR(GetCurrentThread());

  puts_("  clear DR7\n");
  SetDR(GetCurrentThread(), 0, 0);
  const u32 HitsBefore = Hits;
  Sum += TargetA(9);
  const int Cleared = Hits == HitsBefore;
  puts_("  after clear:");
  ShowHit();

  puts_(A ? "PHASE A PASS" : "PHASE A FAIL");
  puts_(Cleared ? " / clear PASS\n" : " / clear FAIL\n");
  Fail |= !A || !Cleared;

  puts_("phase B: DR0 on another thread through SuspendThread/SetThreadContext/ResumeThread\n");
  u32 WorkerId = 0;
  HANDLE Thread = CreateThread(0, 0, Worker, 0, 0, &WorkerId);
  Sleep(50);
  const u32 HitsB = Hits;
  SuspendThread(Thread);
  int OkB = SetDR(Thread, (u64)&TargetB, 0x1);
  ResumeThread(Thread);
  Go = 1;
  WaitForSingleObject(Thread, 10000);
  puts_("  worker tid=");
  dec(WorkerId);
  puts_(" result=");
  dec(WorkerResult);
  puts_(":");
  ShowHit();
  const int B = OkB && Hits == HitsB + 1 && LastAddress == (u64)&TargetB && LastThread == WorkerId && WorkerResult == 37;
  puts_(B ? "PHASE B PASS\n" : "PHASE B FAIL\n");

  if (OtherCode) {
    puts_("other exception seen: ");
    hex(OtherCode);
    puts_("\n");
  }
  puts_(Fail ? "RESULT FAIL\n" : "RESULT PASS\n");
  if (OutFile) {
    CloseHandle(OutFile);
  }
  ExitProcess(Fail ? 1 : 0);
}
