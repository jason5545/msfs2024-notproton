// Cost of one call from x64 code into an ARM64EC function of Wine's (ucrtbase, kernel32, ntdll are ARM64EC
// under Hadron's runtime), against the same work done in x64. Every such call leaves FEX's JIT through
// ExitFunctionEC and comes back through the dispatcher. ns per call, best of 5.
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <windows.h>

#define N 5000000
static double now_ns(void) {
  static LARGE_INTEGER f;
  LARGE_INTEGER c;
  if (!f.QuadPart) QueryPerformanceFrequency(&f);
  QueryPerformanceCounter(&c);
  return c.QuadPart * 1e9 / f.QuadPart;
}
__attribute__((noinline, optnone)) static void *local_memset(void *d, int c, size_t n) {
  unsigned char *p = d;
  while (n--) *p++ = (unsigned char)c;
  return d;
}
static void *(*volatile crt_memset)(void *, int, size_t) = memset;
static void *(*volatile x64_memset)(void *, int, size_t) = local_memset;
static void *(*volatile crt_malloc)(size_t) = malloc;
static void (*volatile crt_free)(void *) = free;
static char buf[64];

int main(void) {
  for (int k = 0; k < 5; k++) {
    const char *name = k == 0 ? "x64-memset8" : k == 1 ? "crt-memset8" : k == 2 ? "crt-malloc-free" : k == 3 ? "GetCurrentThreadId" : "GetLastError";
    double best = 1e30;
    for (int r = 0; r < 5; r++) {
      double t0 = now_ns();
      for (int i = 0; i < N; i++) {
        switch (k) {
        case 0: x64_memset(buf, i, 8); break;
        case 1: crt_memset(buf, i, 8); break;
        case 2: crt_free(crt_malloc(32 + (i & 15))); break;
        case 3: buf[0] += (char)GetCurrentThreadId(); break;
        case 4: buf[1] += (char)GetLastError(); break;
        }
      }
      double t = (now_ns() - t0) / N;
      if (t < best) best = t;
    }
    printf("RESULT %-18s ns=%.2f\n", name, best);
  }
  return 0;
}
