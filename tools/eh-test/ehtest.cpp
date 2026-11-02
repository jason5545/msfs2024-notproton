// ehtest: C++ and SEH exceptions on x64 code under FEX ARM64EC (Wine), no CRT.
// Measures, for each kind of exception, how much stack one throw/catch uses at its deepest point and whether the
// committed stack (TEB StackLimit) keeps growing when the same throw/catch is repeated. Built by build.sh.
typedef unsigned long long u64;
typedef unsigned int u32;
typedef void* HANDLE;

extern "C" {
__declspec(dllimport) HANDLE __stdcall GetStdHandle(u32);
__declspec(dllimport) int __stdcall WriteFile(HANDLE, const void*, u32, u32*, void*);
__declspec(dllimport) void __stdcall ExitProcess(u32);
__declspec(dllimport) u32 __stdcall GetCurrentThreadId(void);
__declspec(dllimport) void* __stdcall ConvertThreadToFiber(void*);
__declspec(dllimport) void* __stdcall CreateFiber(u64, void(__stdcall*)(void*), void*);
__declspec(dllimport) void __stdcall SwitchToFiber(void*);
__declspec(dllimport) HANDLE __stdcall CreateThread(void*, u64, u32(__stdcall*)(void*), void*, u32, u32*);
__declspec(dllimport) u32 __stdcall WaitForSingleObject(HANDLE, u32);
__declspec(dllimport) void __stdcall RaiseException(u32, u32, u32, const u64*);
__declspec(dllimport) HANDLE __stdcall CreateFileA(const char*, u32, u32, void*, u32, u32, HANDLE);
__declspec(dllimport) const char* __stdcall GetCommandLineA(void);
__declspec(dllimport) void* __stdcall AddVectoredExceptionHandler(u32, long(__stdcall*)(void*));
__declspec(dllimport) HANDLE __stdcall GetCurrentThread(void);
__declspec(dllimport) int __stdcall SetThreadContext(HANDLE, const void*);
__declspec(dllimport) void __cdecl qsort(void*, u64, u64, int(__cdecl*)(const void*, const void*));
}

// clang's type descriptors point at type_info's vtable; nothing here uses RTTI, so any symbol will do.
asm(".section .rdata,\"dr\"\n"
    ".globl \"??_7type_info@@6B@\"\n"
    "\"??_7type_info@@6B@\":\n"
    ".quad 0\n"
    ".text\n");

static HANDLE Out, File;

static void put(const char* s) {
  u32 n = 0, w;
  while (s[n]) n++;
  WriteFile(Out, s, n, &w, 0);
  if (File) WriteFile(File, s, n, &w, 0);
}

static void hex(u64 v) {
  char b[19];
  b[0] = '0'; b[1] = 'x';
  for (int i = 0; i < 16; i++) b[2 + i] = "0123456789abcdef"[(v >> ((15 - i) * 4)) & 0xf];
  b[18] = 0;
  put(b);
}

static void dec(u64 v) {
  char b[24];
  int i = 23;
  b[i] = 0;
  do { b[--i] = '0' + v % 10; v /= 10; } while (v);
  put(b + i);
}

static u64 teb_qword(u32 off) {
  u64 v;
  asm volatile("movq %%gs:(%1), %0" : "=r"(v) : "r"((u64)off));
  return v;
}
static u64 stack_base() { return teb_qword(0x08); }
static u64 stack_limit() { return teb_qword(0x10); }
static u64 rsp() {
  u64 v;
  asm volatile("movq %%rsp, %0" : "=r"(v));
  return v;
}

static const u64 Pattern = 0xA5A5A5A55A5A5A5AULL;
static volatile int Sink;

// --- the exceptions -------------------------------------------------------------------------------

__declspec(noinline) static void thrower(int v) {
  if (Sink >= 0) throw v;
}

__declspec(noinline) static int cxx_once() {
  try {
    thrower(7);
  } catch (int v) {
    return v;
  }
  return -1;
}

__declspec(noinline) static int cxx_nested() {
  try {
    try {
      thrower(1);
    } catch (int v) {
      thrower(v + 1); // throw from inside a catch block
    }
  } catch (int v) {
    return v;
  }
  return -1;
}

__declspec(noinline) static int cxx_rethrow() {
  try {
    try {
      thrower(5);
    } catch (...) {
      throw;
    }
  } catch (int v) {
    return v;
  }
  return -1;
}

static int __cdecl throwing_compare(const void*, const void*) {
  thrower(9);
  return 0;
}

// Thrown in x64 code called back from ucrtbase's qsort (ARM64EC on Wine), caught in x64 code above it.
__declspec(noinline) static int cxx_through_arm64ec() {
  int a[4] = {4, 3, 2, 1};
  try {
    qsort(a, 4, sizeof(int), throwing_compare);
  } catch (int v) {
    return v;
  }
  return -1;
}

__declspec(noinline) static int seh_raise() {
  __try {
    RaiseException(0xE0001234, 0, 0, 0);
  } __except (1) {
    return 3;
  }
  return -1;
}

__declspec(noinline) static void null_write() {
  *(volatile int*)(u64)Sink = 1; // Sink is 0: NULL write, access violation raised from JIT code
}

// clang's __try only covers calls, so the faulting store lives in its own function.
__declspec(noinline) static int seh_av() {
  __try {
    null_write();
  } __except (1) {
    return 4;
  }
  return -1;
}

struct Case {
  const char* Name;
  int (*Fn)();
  int Expect;
};

static const Case Cases[] = {
  {"cxx_once", cxx_once, 7},
  {"cxx_nested", cxx_nested, 2},
  {"cxx_rethrow", cxx_rethrow, 5},
  {"cxx_through_arm64ec", cxx_through_arm64ec, 9},
  {"seh_raise", seh_raise, 3},
  {"seh_av", seh_av, 4},
};

// --- measuring ---------------------------------------------------------------------------------------

static const u64 FillBytes = 384 * 1024;
static int Failures;

// Fills the stack below the caller with a pattern, runs Fn once and returns how deep below `Top` it wrote.
__declspec(noinline) static u64 peak_once(int (*Fn)(), int Expect, u64 Top) {
  volatile u64* p = (volatile u64*)(Top - 256);
  volatile u64* end = (volatile u64*)(Top - FillBytes);
  for (; p > end; p--) *p = Pattern; // descending, so each guard page is touched in order
  int r = Fn();
  if (r != Expect) Failures++;
  u64* q = (u64*)(Top - FillBytes) + 1; // the loop above stops just above `end`
  while ((u64)q < Top - 256 && *q == Pattern) q++;
  return Top - (u64)q;
}

static bool SkipAV;

static bool has_arg(const char* word) {
  const char* s = GetCommandLineA();
  for (; *s; s++) {
    const char *a = s, *b = word;
    while (*b && *a == *b) { a++; b++; }
    if (!*b) return true;
  }
  return false;
}

static void run_cases(const char* where, int iterations) {
  for (const Case& c : Cases) {
    if (SkipAV && c.Fn == seh_av) continue;
    u64 top = rsp();
    u64 peak = peak_once(c.Fn, c.Expect, top);
    u64 rsp0 = rsp(), lim0 = stack_limit();
    int bad = 0;
    for (int i = 0; i < iterations; i++) {
      if (c.Fn() != c.Expect) bad++;
    }
    u64 rsp1 = rsp(), lim1 = stack_limit();
    u64 peak2 = peak_once(c.Fn, c.Expect, top);
    Failures += bad;
    put(where); put(" "); put(c.Name);
    put(": peak "); dec(peak); put(" B, after loop "); dec(peak2);
    put(" B; "); dec(iterations); put(" runs, "); dec(bad); put(" wrong");
    put("; rsp "); hex(rsp0); put(rsp0 == rsp1 ? " same" : " CHANGED "); if (rsp0 != rsp1) hex(rsp1);
    put("; StackLimit "); hex(lim0); put(" -> "); hex(lim1);
    put("\n");
  }
}

// --- x86 execute breakpoint (DR0), hit once per call like the game's main loop does once per frame -------------

struct alignas(16) Context64 {
  u64 Home[6];
  u32 ContextFlags, MxCsr;
  unsigned short Seg[6];
  u32 EFlags;
  u64 Dr0, Dr1, Dr2, Dr3, Dr6, Dr7;
  u64 Gpr[16];
  u64 Rip;
  unsigned char Rest[0x4d0 - 0x100];
};
static_assert(sizeof(Context64) == 0x4d0, "CONTEXT size");
static_assert(__builtin_offsetof(Context64, Dr0) == 0x48, "Dr0 offset");
static_assert(__builtin_offsetof(Context64, Rip) == 0xf8, "Rip offset");

struct ExceptionPointers {
  u32* Record; // ExceptionCode is the first field
  Context64* Context;
};

static volatile u64 DrHits;
static u64 DrTarget;

__declspec(noinline) static int dr_target(int x) {
  return x + Sink + 1;
}

static long __stdcall dr_handler(void* p) {
  auto* ep = (ExceptionPointers*)p;
  if (*ep->Record == 0x80000004 && ep->Context->Rip == DrTarget) {
    DrHits++;
    return -1; // EXCEPTION_CONTINUE_EXECUTION
  }
  return 0;
}

static bool set_dr0(u64 addr, u64 dr7) {
  Context64 c = {};
  c.ContextFlags = 0x00100010; // CONTEXT_DEBUG_REGISTERS
  c.Dr0 = addr;
  c.Dr7 = dr7;
  return SetThreadContext(GetCurrentThread(), &c) != 0;
}

__declspec(noinline) static int dr_loop() {
  int bad = 0;
  for (int i = 0; i < 1000; i++)
    if (dr_target(i) != i + 1) bad++;
  return bad;
}

static void dr_case(const char* where) {
  DrTarget = (u64)&dr_target;
  if (!set_dr0(DrTarget, 0x1)) {
    put(where); put(" dr_exec: SetThreadContext failed\n");
    Failures++;
    return;
  }
  u64 top = rsp();
  u64 hits0 = DrHits;
  // peak of one hit
  volatile u64* p = (volatile u64*)(top - 256);
  volatile u64* end = (volatile u64*)(top - FillBytes);
  for (; p > end; p--) *p = Pattern;
  int one = dr_target(41);
  u64* q = (u64*)(top - FillBytes) + 1;
  while ((u64)q < top - 256 && *q == Pattern) q++;
  u64 peak = top - (u64)q;
  u64 rsp0 = rsp(), lim0 = stack_limit();
  int bad = (one != 42);
  for (int i = 0; i < 20; i++) bad += dr_loop();
  u64 rsp1 = rsp(), lim1 = stack_limit();
  u64 hits = DrHits - hits0;
  set_dr0(0, 0);
  if (dr_target(1) != 2) bad++;
  u64 after_clear = DrHits - hits0 - hits;
  if (hits != 20001 || after_clear) bad++;
  Failures += bad;
  put(where); put(" dr_exec: hits "); dec(hits); put(" (want 20001), after clear "); dec(after_clear);
  put("; peak "); dec(peak); put(" B; rsp "); hex(rsp0); put(rsp0 == rsp1 ? " same" : " CHANGED "); if (rsp0 != rsp1) hex(rsp1);
  put("; StackLimit "); hex(lim0); put(" -> "); hex(lim1); put(bad ? " FAIL\n" : " ok\n");
}

// The handler emulates the whole function instead of letting it run: it sets the return value and jumps straight to
// the return address (RIP = [RSP], RSP += 8). MSFS 2024's four-slot breakpoint handler resumes somewhere other than the
// breakpoint address the same way. On x86 the RF flag only covers the first instruction after the handler returns, so
// the next call of the same function must hit the breakpoint again.
static volatile u64 DrSkipHits;
static u64 DrSkipTarget;

__declspec(noinline) static int dr_skip_target(int x) {
  return x + Sink + 1; // only runs if the breakpoint was missed
}

static long __stdcall dr_skip_handler(void* p) {
  auto* ep = (ExceptionPointers*)p;
  Context64* c = ep->Context;
  if (*ep->Record == 0x80000004 && c->Rip == DrSkipTarget) {
    DrSkipHits++;
    c->Gpr[0] = 7777;                 // RAX: the emulated return value
    c->Rip = *(u64*)c->Gpr[4];        // return address
    c->Gpr[4] += 8;                   // RSP: pop it
    return -1;
  }
  return 0;
}

static void dr_skip_case(const char* where) {
  DrSkipTarget = (u64)&dr_skip_target;
  if (!set_dr0(DrSkipTarget, 0x1)) {
    put(where); put(" dr_skip: SetThreadContext failed\n");
    Failures++;
    return;
  }
  u64 hits0 = DrSkipHits;
  u64 rsp0 = rsp();
  int native = 0, other = 0, first_native = -1;
  for (int i = 0; i < 1000; i++) {
    int r = dr_skip_target(i);
    if (r == i + 1) {
      native++;
      if (first_native < 0) first_native = i;
    } else if (r != 7777) {
      other++;
    }
  }
  u64 rsp1 = rsp();
  set_dr0(0, 0);
  u64 hits = DrSkipHits - hits0;
  int bad = native || other || hits != 1000 || rsp0 != rsp1;
  Failures += bad;
  put(where); put(" dr_skip: hits "); dec(hits); put(" (want 1000), ran natively "); dec(native);
  if (first_native >= 0) { put(" (first at call "); dec(first_native); put(")"); }
  put(", other "); dec(other); put("; rsp "); put(rsp0 == rsp1 ? "same" : "CHANGED"); put(bad ? " FAIL\n" : " ok\n");
}

static bool RunDR;

static void* MainFiber;
static void* WorkFiber;

static void __stdcall fiber_proc(void*) {
  put("fiber: StackBase "); hex(stack_base()); put(" StackLimit "); hex(stack_limit()); put("\n");
  run_cases("fiber", 2000);
  if (RunDR) dr_case("fiber");
  SwitchToFiber(MainFiber);
}

static u32 __stdcall thread_proc(void*) {
  put("thread "); dec(GetCurrentThreadId()); put(": StackBase "); hex(stack_base()); put(" StackLimit ");
  hex(stack_limit()); put("\n");
  run_cases("thread", 2000);
  if (RunDR) dr_case("thread");
  return 0;
}

extern "C" void Entry() {
  Out = GetStdHandle((u32)-11);
  File = CreateFileA("ehtest-out.txt", 0x40000000, 1, 0, 2, 0x80, 0); // GENERIC_WRITE, CREATE_ALWAYS
  if (File == (HANDLE)(long long)-1) File = 0;
  put("ehtest: main tid "); dec(GetCurrentThreadId()); put(" StackBase "); hex(stack_base()); put(" StackLimit ");
  hex(stack_limit()); put(" rsp "); hex(rsp()); put("\n");

  SkipAV = has_arg("skipav");
  put(SkipAV ? "seh_av skipped\n" : "");
  if (has_arg(" unhandled")) {
    put("unhandled NULL write from x64 code\n");
    null_write(); // nobody catches this: the dispatcher walks to the top of the thread
  }
  RunDR = has_arg(" dr");
  if (RunDR) {
    AddVectoredExceptionHandler(1, dr_handler);
    AddVectoredExceptionHandler(1, dr_skip_handler);
  }
  if (has_arg(" onlyskip")) {
    dr_skip_case("main");
    put(Failures ? "RESULT FAIL\n" : "RESULT PASS\n");
    ExitProcess(Failures ? 1 : 0);
  }
  run_cases("main", 2000);
  if (RunDR) {
    dr_case("main");
    dr_skip_case("main");
  }

  u32 tid;
  HANDLE t = CreateThread(0, 0, thread_proc, 0, 0, &tid);
  WaitForSingleObject(t, 0xFFFFFFFF);

  MainFiber = ConvertThreadToFiber(0);
  WorkFiber = CreateFiber(0x180000, fiber_proc, 0);
  SwitchToFiber(WorkFiber);
  put("back on main fiber: StackLimit "); hex(stack_limit()); put("\n");
  run_cases("main-after-fiber", 2000);
  if (RunDR) dr_case("main-after-fiber");

  put(Failures ? "RESULT FAIL\n" : "RESULT PASS\n");
  ExitProcess(Failures ? 1 : 0);
}
