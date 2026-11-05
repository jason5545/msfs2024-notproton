/*
 * smcsub (squark, 11/5): self-modifying code next to code FEX already translated, as a x86-64 PE.
 *
 * Written while looking for the stale translations that made JavaScriptCore's JIT assert under FEX (FTL lazy slow path
 * generated twice, OSR exit compiled twice). FEX tracks writes to translated code by write-protecting the guest pages
 * that hold it; a write fault drops the page's translations and makes the page writable again. Two suspects:
 * - Windows pages are 4 KB, an Apple Silicon host page is 16 KB, and Wine protects a host page with the union of its
 *   four 4 KB pages' protections (ntdll get_host_page_vprot): the fault on one 4 KB page might leave code on the other
 *   three writable without a fault, still running as translated (S1-S8). FEX 0039 passes these.
 * - Code a call returns into, rewritten by the callee while another thread writes data on the page (S9, the JSC
 *   pattern): before squark's FEX patch 0040 a third of the calls ran the old instruction after the call (Highball's
 *   FEX, 0038, 0039); with 0040 none.
 *
 * Each case uses a fresh 64 KB RWX allocation (64 KB aligned, so its first 16 KB is one host page). At the start of
 * each 4 KB page p (0..3): "mov eax, imm32; ret". All four are called (translated, pages write-protected). Then:
 *   S1  a data byte is written on page 0 (offset 0x800, not code), the imm32 of the functions on pages 1-3 is
 *       rewritten with plain stores, and they are called: each must return its new value.
 *   S2  the same, the rewrite with memcpy (on ARM64EC, ucrtbase's native memcpy writes it).
 *   S3  the reference: the functions sit on host pages of their own (offsets 0x4000, 0x8000, 0xc000), the data byte
 *       is written on the first host page; every rewrite then faults on its own host page.
 *   S4  as S1, with the data write on page 3 and the code on pages 0-2.
 *   S5  only page 0's function is called before the rewrite: pages 1-3 hold no translated code and stay writable, so
 *       the host page keeps write access; page 0's function is rewritten and called.
 *   S6  pages 0 and 1 are called, a data byte is written on page 2 (never called), then page 1's function is
 *       rewritten and called.
 *   S7  cross-thread: a writer thread keeps storing data into the same host page (offset 0x800 of each 4 KB page, never
 *       code) while the main thread rewrites the imm32 of page 1's function and calls it, ROUNDS*50 times in one
 *       allocation (as JavaScriptCore's compiler threads write new code next to code the main thread repatches).
 *   S8  as S7, with a jump: page 1 holds "jmp rel32" to one of two targets on page 2 ("mov eax, 1/2; ret"); the main
 *       thread retargets it (as JSC's repatchJump) and calls it; the result must follow the jump.
 *   S9  cross-thread, through a call: page 1 holds F = "mov rax, imm64; call rax; mov eax, imm32; ret", where rax is a
 *       C function of this program that writes a new imm32 into F's "mov eax" (the code F's call returns to) and
 *       returns; F must return the new value. The writer thread keeps storing data next to F all the while (JSC: the
 *       main thread repatches code it returns into, from C++ called by that code, while compiler threads write new
 *       code beside it). ROUNDS*50 calls.
 * Every case runs ROUNDS (default 200) times; ONLY=<n> runs case n alone. Output: one line per case with its stale calls, then
 * "RESULT smcsub stale=<n> calls=<n>" (stale must be 0).
 */
#include <windows.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef int (*fn_t)(void);

static void put_fn(unsigned char *p, int value) {
  p[0] = 0xb8;
  memcpy(p + 1, &value, 4);
  p[5] = 0xc3;
}

static volatile int sink;
static volatile LONG writer_stop;
static unsigned char *volatile writer_base;

static DWORD WINAPI writer(void *arg) {
  (void)arg;
  unsigned v = 0;
  while (!writer_stop) {
    unsigned char *b = writer_base;
    for (int p = 0; p < 4; p++) {
      unsigned char buf[64];
      memset(buf, (int)(v++ & 0xff), sizeof(buf));
      memcpy(b + p * 0x1000 + 0x800, buf, sizeof(buf));
    }
  }
  return 0;
}

static unsigned char *volatile s9_fn;
static volatile int s9_value;
static __attribute__((noinline)) void s9_patch(void) {
  *(volatile int *)(s9_fn + 13) = ++s9_value;
}

static int run_xthread(int which, int rounds, long *calls) {
  unsigned char *base = VirtualAlloc(NULL, 0x10000, MEM_RESERVE | MEM_COMMIT, PAGE_EXECUTE_READWRITE);
  if (!base) {
    printf("VirtualAlloc failed %lu\n", GetLastError());
    exit(2);
  }
  unsigned char *f = base + 0x1000, *t1 = base + 0x2000, *t2 = base + 0x2040;
  put_fn(t1, 1);
  put_fn(t2, 2);
  if (which == 7) {
    put_fn(f, 0);
  } else if (which == 9) {
    // 48 b8 imm64 (mov rax, imm64); ff d0 (call rax); b8 imm32 (mov eax, imm32) at offset 12, imm at 13; c3
    const unsigned long long target = (unsigned long long)(void *)s9_patch;
    f[0] = 0x48;
    f[1] = 0xb8;
    memcpy(f + 2, &target, 8);
    f[10] = 0xff;
    f[11] = 0xd0;
    put_fn(f + 12, 0);
    s9_fn = f;
    s9_value = 0;
  } else {
    const int rel = (int)(t1 - (f + 5));
    f[0] = 0xe9;
    memcpy(f + 1, &rel, 4);
  }
  FlushInstructionCache(GetCurrentProcess(), base, 0x10000);
  writer_base = base;
  writer_stop = 0;
  HANDLE h = CreateThread(NULL, 0, writer, NULL, 0, NULL);
  int stale = 0;
  for (int i = 1; i <= rounds * 50; i++) {
    if (which == 9) {
      const int got = ((fn_t)f)();
      ++*calls;
      if (got != s9_value) {
        if (stale < 3) {
          printf("  S9 call %d: got %d, want %d\n", i, got, s9_value);
        }
        stale++;
      }
      continue;
    }
    sink = ((fn_t)f)();
    int want;
    if (which == 7) {
      want = i;
      *(volatile int *)(f + 1) = i;
    } else {
      unsigned char *to = (i & 1) ? t2 : t1;
      want = (i & 1) ? 2 : 1;
      *(volatile int *)(f + 1) = (int)(to - (f + 5));
    }
    const int got = ((fn_t)f)();
    ++*calls;
    if (got != want) {
      if (stale < 3) {
        printf("  S%d call %d: got %d, want %d\n", which, i, got, want);
      }
      stale++;
    }
  }
  writer_stop = 1;
  WaitForSingleObject(h, INFINITE);
  CloseHandle(h);
  VirtualFree(base, 0, MEM_RELEASE);
  return stale;
}

static int run_case(int which, int rounds, long *calls) {
  int stale = 0;
  for (int r = 0; r < rounds; r++) {
    unsigned char *base = VirtualAlloc(NULL, 0x10000, MEM_RESERVE | MEM_COMMIT, PAGE_EXECUTE_READWRITE);
    if (!base) {
      printf("VirtualAlloc failed %lu\n", GetLastError());
      exit(2);
    }
    const size_t stride = which == 3 ? 0x4000 : 0x1000;
    for (int p = 0; p < 4; p++) {
      put_fn(base + p * stride, 1000 + p);
    }
    FlushInstructionCache(GetCurrentProcess(), base, 0x10000);
    const int called = which == 5 ? 1 : which == 6 ? 2 : 4;
    for (int p = 0; p < called; p++) {
      sink = ((fn_t)(base + p * stride))();
    }
    const int data_page = which == 4 ? 3 : which == 6 ? 2 : 0;
    const int first = which == 4 || which == 5 ? 0 : 1, last = which == 4 ? 2 : which >= 5 ? first : 3;
    if (which != 5) {
      ((volatile unsigned char *)base)[data_page * 0x1000 + 0x800] = (unsigned char)r;
    }
    for (int p = first; p <= last; p++) {
      const int value = 2000 + r * 4 + p;
      unsigned char *at = base + p * stride + 1;
      if (which == 2) {
        memcpy(at, &value, 4);
      } else {
        *(volatile int *)at = value;
      }
    }
    for (int p = first; p <= last; p++) {
      const int want = 2000 + r * 4 + p;
      const int got = ((fn_t)(base + p * stride))();
      ++*calls;
      if (got != want) {
        if (stale < 3) {
          printf("  S%d round %d page %d: got %d, want %d\n", which, r, p, got, want);
        }
        stale++;
      }
    }
    VirtualFree(base, 0, MEM_RELEASE);
  }
  return stale;
}

int main(void) {
  const char *env = getenv("ROUNDS");
  setvbuf(stdout, NULL, _IONBF, 0);
  const int rounds = env ? atoi(env) : 200;
  SYSTEM_INFO si;
  GetSystemInfo(&si);
  printf("page_size=%lu allocation_granularity=%lu rounds=%d\n", si.dwPageSize, si.dwAllocationGranularity, rounds);
  int total = 0;
  long calls = 0;
  const char *only = getenv("ONLY");
  for (int which = 1; which <= 9; which++) {
    if (only && atoi(only) != which) {
      continue;
    }
    long c = 0;
    const int stale = which >= 7 ? run_xthread(which, rounds, &c) : run_case(which, rounds, &c);
    printf("S%d stale=%d calls=%ld\n", which, stale, c);
    total += stale;
    calls += c;
  }
  printf("RESULT smcsub stale=%d calls=%ld\n", total, calls);
  // HOLD_END=<ms>: wait before exiting (to copy FEX's live stats file, FEX_PROFILESTATS=1)
  if ((env = getenv("HOLD_END"))) {
    printf("PHASE done\n");
    fflush(stdout);
    Sleep(atoi(env));
  }
  return total ? 1 : 0;
}
