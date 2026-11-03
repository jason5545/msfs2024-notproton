// smcmsfs.c: the ways Microsoft Flight Simulator 2024 rewrites memory that holds its own x86-64 code, as small timed
// loops, to compare FEX's self-modifying-code handling (write-protected pages, hot pages, Highball's replay, retire and
// skip-same) without opening the game. Every call's result is checked, so a translation that runs old code shows up as
// a stale call. Output: one line per scenario, then RESULT PASS or RESULT FAIL.
//
//   S1  data beside code: write a counter on the page of a 100-instruction function, call it, repeat
//   S1t the same with a second thread writing data on that page all the time
//   S2  rewrite bytes that are already there (a 16-byte instruction run of the function), call it, repeat
//   S3  decrypt, run, encrypt: 8 functions of 250 instructions on 2 pages, XOR'd to plain, called, XOR'd back
//   S4  two versions in turn: one function's immediate flips each round; three more functions share its page
//   S5  translate fresh code (2,000 functions per round) while another thread allocates, protects and frees
//       read-write memory; and the allocating thread's rate alone and during the translation
//   S6  how fast code runs: a 4,000-instruction loop on a read-execute page, a cold RWX page and a hot RWX page
//   S7  cross-thread patch race: a fresh page each round, the reader translating it while the writer patches its
//       immediate once; a patch the reader never sees within 500 ms is a miss (x86 makes it visible)
//   S8  a write to a page the program turned back from RWX to RX after running code on it must fault
//
// usage: smcmsfs.exe [scale] [scenarios]   scale multiplies the iteration counts (default 1); scenarios is a list like
// "S1,S4" (default all). Build: tools/smc-msfs/build.sh (llvm-mingw).
#include <windows.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef int (*fn_t)(void);
static double Freq;
static int Failures;
static const char* Only;
static int Scale = 1;

static double Now(void) {
  LARGE_INTEGER t;
  QueryPerformanceCounter(&t);
  return (double)t.QuadPart / Freq;
}
static int Want(const char* Name) {
  if (!Only) return 1;
  const char* p = Only;
  size_t n = strlen(Name);
  while ((p = strstr(p, Name))) {
    if ((p == Only || p[-1] == ',') && (p[n] == ',' || p[n] == 0)) return 1;
    p += n;
  }
  return 0;
}
static uint8_t* AllocRWX(size_t Size) {
  uint8_t* p = VirtualAlloc(NULL, Size, MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE);
  if (!p) {
    printf("VirtualAlloc %zu failed %lu\n", Size, GetLastError());
    exit(3);
  }
  return p;
}

// mov eax, k ; n x add eax, 1 ; ret  -> k + n      (5 + 3n + 1 bytes)
static size_t EmitAdd(uint8_t* p, uint32_t k, int n) {
  uint8_t* s = p;
  *p++ = 0xB8;
  memcpy(p, &k, 4);
  p += 4;
  for (int i = 0; i < n; i++) {
    *p++ = 0x83; *p++ = 0xC0; *p++ = 0x01;
  }
  *p++ = 0xC3;
  return (size_t)(p - s);
}
// nop x3 ; mov eax, imm32 (imm at +4, 4-byte aligned) ; n x add eax, 0 ; ret  -> imm
static size_t EmitImm(uint8_t* p, uint32_t imm, int n) {
  uint8_t* s = p;
  *p++ = 0x90; *p++ = 0x90; *p++ = 0x90;
  *p++ = 0xB8;
  memcpy(p, &imm, 4);
  p += 4;
  for (int i = 0; i < n; i++) {
    *p++ = 0x83; *p++ = 0xC0; *p++ = 0x00;
  }
  *p++ = 0xC3;
  return (size_t)(p - s);
}
// mov ecx, iters ; xor eax, eax ; L: add eax, ecx ; xor eax, 0x55 ; imul eax, eax, 3 ; dec ecx ; jnz L ; ret
static size_t EmitLoop(uint8_t* p, uint32_t iters) {
  static const uint8_t body[] = {0x31, 0xC0, 0x01, 0xC8, 0x83, 0xF0, 0x55, 0x6B, 0xC0, 0x03, 0xFF, 0xC9, 0x75, 0xF4, 0xC3};
  p[0] = 0xB9;
  memcpy(p + 1, &iters, 4);
  memcpy(p + 5, body, sizeof(body));
  return 5 + sizeof(body);
}
static int LoopExpect(uint32_t iters) {
  uint32_t a = 0;
  for (uint32_t c = iters; c; c--) {
    a += c;
    a ^= 0x55;
    a *= 3;
  }
  return (int)a;
}

static void Report(const char* Name, const char* What, int N, double Seconds, int Stale) {
  printf("%-3s %-44s n=%-6d us_per=%9.2f stale=%d\n", Name, What, N, Seconds * 1e6 / N, Stale);
  if (Stale) Failures++;
  fflush(stdout);
}

// ---------------------------------------------------------------- S1, S1t
static volatile LONG StopWriter;
static volatile uint64_t* WriterSlot;
static DWORD WINAPI DataWriter(LPVOID Arg) {
  (void)Arg;
  uint64_t i = 0;
  while (!StopWriter) {
    *WriterSlot = ++i;
  }
  return 0;
}
static void S1(int Threaded) {
  const int n = 20000 * Scale;
  uint8_t* page = AllocRWX(4096);
  EmitAdd(page, 1, 100);
  fn_t f = (fn_t)page;
  volatile uint64_t* data = (volatile uint64_t*)(page + 2048);
  HANDLE th = NULL;
  if (Threaded) {
    StopWriter = 0;
    WriterSlot = (volatile uint64_t*)(page + 3072);
    th = CreateThread(NULL, 0, DataWriter, NULL, 0, NULL);
    Sleep(5);
  }
  int stale = 0;
  double t0 = Now();
  for (int i = 0; i < n; i++) {
    *data = (uint64_t)i;
    if (f() != 101) stale++;
  }
  double s = Now() - t0;
  if (th) {
    StopWriter = 1;
    WaitForSingleObject(th, INFINITE);
    CloseHandle(th);
  }
  Report(Threaded ? "S1t" : "S1", Threaded ? "data beside code, data-writing thread" : "data beside code", n, s, stale);
}

// ---------------------------------------------------------------- S2
static void S2(void) {
  const int n = 20000 * Scale;
  uint8_t* page = AllocRWX(4096);
  EmitAdd(page, 3, 100);
  fn_t f = (fn_t)page;
  uint8_t saved[16];
  memcpy(saved, page + 32, 16);
  int stale = 0;
  double t0 = Now();
  for (int i = 0; i < n; i++) {
    // the bytes already there, as one 16-byte store (copy protections write code back unchanged)
    __asm__ volatile("movdqu (%1), %%xmm0\n\tmovdqu %%xmm0, (%0)" ::"r"(page + 32), "r"(saved) : "xmm0", "memory");
    if (f() != 103) stale++;
  }
  Report("S2", "rewrite unchanged code bytes", n, Now() - t0, stale);
}

// ---------------------------------------------------------------- S3
static void S3(void) {
  enum { FUNCS = 8, BODY = 5 + 250 * 3 + 1, PER_PAGE = 4 };
  const int rounds = 1000 * Scale;
  uint8_t* pages = AllocRWX(4096 * (FUNCS / PER_PAGE));
  uint8_t* fn[FUNCS];
  for (int j = 0; j < FUNCS; j++) {
    fn[j] = pages + (j / PER_PAGE) * 4096 + (j % PER_PAGE) * 1024;
    EmitAdd(fn[j], 1000 * j, 250);
    for (int b = 0; b < BODY; b++) fn[j][b] ^= 0xA5;
  }
  int stale = 0;
  double t0 = Now();
  for (int r = 0; r < rounds; r++) {
    for (int j = 0; j < FUNCS; j++) {
      for (int b = 0; b < BODY; b++) fn[j][b] ^= 0xA5;
      if (((fn_t)fn[j])() != 1000 * j + 250) stale++;
      for (int b = 0; b < BODY; b++) fn[j][b] ^= 0xA5;
    }
  }
  Report("S3", "decrypt, run, encrypt (per function)", rounds * FUNCS, Now() - t0, stale);
}

// ---------------------------------------------------------------- S4
static void S4(void) {
  const int rounds = 2000 * Scale;
  uint8_t* page = AllocRWX(4096);
  uint8_t* fn[4];
  for (int j = 0; j < 4; j++) {
    fn[j] = page + j * 1024;
    EmitAdd(fn[j], 100 * j, 250);
  }
  int stale = 0;
  double t0 = Now();
  for (int r = 0; r < rounds; r++) {
    const uint32_t k = 40 + (r & 1);
    memcpy(fn[0] + 1, &k, 4);
    if (((fn_t)fn[0])() != (int)k + 250) stale++;
    for (int j = 1; j < 4; j++) {
      if (((fn_t)fn[j])() != 100 * j + 250) stale++;
    }
  }
  Report("S4", "two versions in turn, three neighbours", rounds, Now() - t0, stale);
}

// ---------------------------------------------------------------- S5
static volatile LONG StopChurn;
static volatile LONG64 ChurnOps;
static DWORD WINAPI Churn(LPVOID Arg) {
  (void)Arg;
  while (!StopChurn) {
    uint8_t* p = VirtualAlloc(NULL, 65536, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
    if (!p) continue;
    p[0] = 1;
    p[40000] = 2;
    DWORD old;
    VirtualProtect(p, 65536, PAGE_READONLY, &old);
    VirtualProtect(p, 65536, PAGE_READWRITE, &old);
    VirtualFree(p, 0, MEM_RELEASE);
    InterlockedAdd64(&ChurnOps, 4);
  }
  return 0;
}
static double TranslateRound(int* Stale) {
  enum { N = 2000, SZ = 32 };
  uint8_t* code = AllocRWX(N * SZ);
  for (int j = 0; j < N; j++) {
    uint8_t* p = code + j * SZ;
    p[0] = 0xB8;
    uint32_t k = (uint32_t)j;
    memcpy(p + 1, &k, 4);
    for (int i = 0; i < 8; i++) {
      p[5 + 3 * i] = 0x83; p[6 + 3 * i] = 0xC0; p[7 + 3 * i] = (uint8_t)((j + i) & 0x3F);
    }
    p[29] = 0xC3;
  }
  double t0 = Now();
  for (int j = 0; j < N; j++) {
    int want = j;
    for (int i = 0; i < 8; i++) want += (j + i) & 0x3F;
    if (((fn_t)(code + j * SZ))() != want) (*Stale)++;
  }
  double s = Now() - t0;
  VirtualFree(code, 0, MEM_RELEASE);
  return s;
}
static void S5(void) {
  const int rounds = 10 * Scale;
  int stale = 0;
  double alone = 0, busy = 0;
  for (int r = 0; r < rounds; r++) alone += TranslateRound(&stale);
  // the allocating thread alone
  StopChurn = 0;
  ChurnOps = 0;
  HANDLE th = CreateThread(NULL, 0, Churn, NULL, 0, NULL);
  double c0 = Now();
  Sleep(300);
  double churn_alone = (double)ChurnOps / (Now() - c0);
  LONG64 ops0 = ChurnOps;
  double b0 = Now();
  for (int r = 0; r < rounds; r++) busy += TranslateRound(&stale);
  double churn_busy = (double)(ChurnOps - ops0) / (Now() - b0);
  StopChurn = 1;
  WaitForSingleObject(th, INFINITE);
  CloseHandle(th);
  printf("S5  translate 2000 functions: alone %.2f ms/round, beside memory calls %.2f ms/round; memory calls %.0f/s alone, "
         "%.0f/s during translation; stale=%d\n",
         alone * 1e3 / rounds, busy * 1e3 / rounds, churn_alone, churn_busy, stale);
  if (stale) Failures++;
  fflush(stdout);
}

// ---------------------------------------------------------------- S6
static double TimeLoop(fn_t f, int Want_, int N, int* Stale) {
  f();
  double t0 = Now();
  for (int i = 0; i < N; i++) {
    if (f() != Want_) (*Stale)++;
  }
  return (Now() - t0) * 1e6 / N;
}
static void S6(void) {
  const uint32_t iters = 1000; // 5 instructions an iteration
  const int n = 2000 * Scale;
  const int want = LoopExpect(iters);
  int stale = 0;
  // read-execute page
  uint8_t* rx = VirtualAlloc(NULL, 4096, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
  EmitLoop(rx, iters);
  DWORD old;
  VirtualProtect(rx, 4096, PAGE_EXECUTE_READ, &old);
  double t_rx = TimeLoop((fn_t)rx, want, n, &stale);
  // cold RWX page: code only, never written after it ran
  uint8_t* cold = AllocRWX(4096);
  EmitLoop(cold, iters);
  double t_cold = TimeLoop((fn_t)cold, want, n, &stale);
  // hot RWX page: data written beside the code many times after it ran
  uint8_t* hot = AllocRWX(4096);
  EmitLoop(hot, iters);
  volatile uint32_t* d = (volatile uint32_t*)(hot + 2048);
  for (int i = 0; i < 64; i++) {
    *d = (uint32_t)i;
    if (((fn_t)hot)() != want) stale++;
  }
  double t_hot = TimeLoop((fn_t)hot, want, n, &stale);
  printf("S6  4000-instruction loop: RX %.2f us, cold RWX %.2f us, hot RWX %.2f us per call (hot/RX %.2f); stale=%d\n", t_rx,
         t_cold, t_hot, t_hot / t_rx, stale);
  if (stale) Failures++;
  fflush(stdout);
}

// ---------------------------------------------------------------- S7
static volatile LONG S7Round = -1, S7Started = -1, S7Stop;
static volatile uint8_t* volatile S7Fn;
static volatile int S7Last;
static DWORD WINAPI S7Reader(LPVOID Arg) {
  (void)Arg;
  LONG seen = -1;
  while (!S7Stop) {
    LONG r = S7Round;
    if (r == seen) {
      YieldProcessor();
      continue;
    }
    seen = r;
    fn_t f = (fn_t)S7Fn;
    S7Started = r;
    while (S7Round == r && !S7Stop) {
      S7Last = f();
    }
  }
  return 0;
}
static void S7(void) {
  const int rounds = 1500 * Scale;
  uint8_t* region = AllocRWX((size_t)rounds * 4096);
  HANDLE th = CreateThread(NULL, 0, S7Reader, NULL, 0, NULL);
  int misses = 0, done = 0;
  double worst = 0, t0 = Now();
  for (int r = 0; r < rounds && misses < 20; r++) {
    uint8_t* p = region + (size_t)r * 4096;
    EmitImm(p, 1000000 + (uint32_t)r, 300);
    S7Last = 0;
    S7Fn = p;
    MemoryBarrier();
    S7Round = r;
    while (S7Started != r) YieldProcessor();
    // patch at a different point of the reader's first translation each round
    for (volatile int spin = 0; spin < (r % 97) * 40; spin++) {
    }
    const uint32_t v = 2000000 + (uint32_t)r;
    *(volatile uint32_t*)(p + 4) = v;
    double w0 = Now();
    while (S7Last != (int)v && Now() - w0 < 0.5) YieldProcessor();
    double w = Now() - w0;
    if (S7Last != (int)v) misses++;
    else if (w > worst) worst = w;
    done++;
  }
  S7Stop = 1;
  S7Round = -2;
  WaitForSingleObject(th, INFINITE);
  CloseHandle(th);
  printf("S7  cross-thread patch of a page in translation   rounds=%d misses=%d slowest_seen_ms=%.2f total_s=%.2f\n", done,
         misses, worst * 1e3, Now() - t0);
  if (misses) Failures++;
  fflush(stdout);
}

// ---------------------------------------------------------------- S8
static volatile uint8_t* S8Target;
static volatile LONG S8Faults;
static LONG CALLBACK S8Handler(EXCEPTION_POINTERS* E) {
  if (E->ExceptionRecord->ExceptionCode == EXCEPTION_ACCESS_VIOLATION &&
      E->ExceptionRecord->ExceptionInformation[1] == (ULONG_PTR)S8Target) {
    S8Faults++;
    E->ContextRecord->Rip += 3; // movb $0x90, (%rcx)
    return EXCEPTION_CONTINUE_EXECUTION;
  }
  return EXCEPTION_CONTINUE_SEARCH;
}
static void S8(void) {
  uint8_t* page = AllocRWX(4096);
  EmitAdd(page, 5, 10);
  int stale = ((fn_t)page)() != 15;
  DWORD old;
  VirtualProtect(page, 4096, PAGE_EXECUTE_READ, &old);
  void* h = AddVectoredExceptionHandler(1, S8Handler);
  S8Target = page + 2048;
  __asm__ volatile("movb $0x90, (%0)" ::"c"(S8Target) : "memory");
  RemoveVectoredExceptionHandler(h);
  MEMORY_BASIC_INFORMATION mbi;
  VirtualQuery(page, &mbi, sizeof(mbi));
  const int ok = S8Faults == 1 && page[2048] == 0 && !stale;
  printf("S8  write to a page turned RWX -> RX after its code ran: faults=%ld byte=0x%02x protect=0x%lx (old 0x%lx) %s\n",
         S8Faults, page[2048], mbi.Protect, old, ok ? "ok" : "WRONG (want 1 fault, byte 0x00, 0x20)");
  if (!ok) Failures++;
  fflush(stdout);
}

int main(int argc, char** argv) {
  LARGE_INTEGER f;
  QueryPerformanceFrequency(&f);
  Freq = (double)f.QuadPart;
  if (argc > 1) Scale = atoi(argv[1]) > 0 ? atoi(argv[1]) : 1;
  if (argc > 2) Only = argv[2];
  setvbuf(stdout, NULL, _IONBF, 0);
  printf("smcmsfs scale=%d\n", Scale);
  if (Want("S1")) S1(0);
  if (Want("S1t")) S1(1);
  if (Want("S2")) S2();
  if (Want("S3")) S3();
  if (Want("S4")) S4();
  if (Want("S5")) S5();
  if (Want("S6")) S6();
  if (Want("S7")) S7();
  if (Want("S8")) S8();
  printf("RESULT %s\n", Failures ? "FAIL" : "PASS");
  return Failures;
}
