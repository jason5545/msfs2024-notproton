// Cost of switching MXCSR around a call, the way a WASM host may set its own floating-point mode when it
// enters a module and restore the caller's when it leaves (FEX turns LDMXCSR into an FPCR write, which
// Highball's FEX patch 0012 skips only when nothing changes). ns per iteration:
//   plain   call only
//   same    LDMXCSR of the value already there before and after the call
//   toggle  LDMXCSR to another rounding/FTZ/DAZ mode before the call, back after it
#include <stdio.h>
#include <windows.h>
#include <xmmintrin.h>

__attribute__((noinline)) static double work(double x) { return x * 1.0000001 + 0.5; }

static double now_ns(void) {
  static LARGE_INTEGER f;
  LARGE_INTEGER c;
  if (!f.QuadPart) QueryPerformanceFrequency(&f);
  QueryPerformanceCounter(&c);
  return c.QuadPart * 1e9 / f.QuadPart;
}

int main(void) {
  const int N = 20000000;
  unsigned base = _mm_getcsr(), other = base | 0x8040; // FTZ and DAZ on
  volatile double x = 1;
  for (int mode = 0; mode < 3; mode++) {
    double t0 = now_ns();
    for (int i = 0; i < N; i++) {
      if (mode == 1) _mm_setcsr(base);
      if (mode == 2) _mm_setcsr(other);
      x = work(x);
      if (mode) _mm_setcsr(base);
    }
    printf("RESULT %-6s ns_per_call=%.2f\n", mode == 0 ? "plain" : mode == 1 ? "same" : "toggle", (now_ns() - t0) / N);
  }
  return 0;
}
