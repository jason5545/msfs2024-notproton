// Code patterns the FBW A380X systems (Rust -> wasm -> wasm2c -> x64) are made of, each timed alone so
// the ratio FEX / native shows which ones FEX translates worst. Same source for the x64 PE (under
// FEX) and arm64 macOS. ns per iteration, best of 5.
//
//   patterns[.exe] [name...]
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#ifdef _WIN32
#include <windows.h>
static double now_ns(void) {
  static LARGE_INTEGER f;
  LARGE_INTEGER c;
  if (!f.QuadPart) QueryPerformanceFrequency(&f);
  QueryPerformanceCounter(&c);
  return c.QuadPart * 1e9 / f.QuadPart;
}
#else
#include <time.h>
static double now_ns(void) {
  struct timespec t;
  clock_gettime(CLOCK_MONOTONIC, &t);
  return t.tv_sec * 1e9 + t.tv_nsec;
}
#endif

#define N 20000000
#define NOINLINE __attribute__((noinline))
static volatile double vd = 1.0;
static volatile uint64_t vu = 12345;

NOINLINE static double leaf(double x) { return x * 0.999 + 0.001; }
// Callee that saves registers, like most Rust functions on the Windows x64 ABI (xmm6-15 callee-saved)
NOINLINE static double saver(double a, double b) {
  double t[8];
  for (int i = 0; i < 8; i++) t[i] = a * i + b;
  double s = 0;
  for (int i = 0; i < 8; i++) s += t[i] * t[(i + 3) & 7];
  return s * 1e-9 + a;
}
typedef double (*fn_t)(double);
NOINLINE static double l0(double x) { return x * 0.999; }
NOINLINE static double l1(double x) { return x + 0.001; }
NOINLINE static double l2(double x) { return x * 0.998 + 0.002; }
NOINLINE static double l3(double x) { return x - 0.0005; }
static fn_t tab[4] = {l0, l1, l2, l3};

static double p_call(void) { double x = vd; for (int i = 0; i < N; i++) x = leaf(x); return x; }
static double p_saver(void) { double x = vd; for (int i = 0; i < N; i++) x = saver(x, 0.5); return x; }
static double p_icall(void) { double x = vd; for (int i = 0; i < N; i++) x = tab[i & 3](x); return x; }
static double p_fcmp(void) { // ucomisd + branch (NaN check then compare), as Rust's f64 comparisons compile
  double x = vd, s = 0;
  for (int i = 0; i < N; i++) { x = x * 1.0000001; if (x != x) s += 1; else if (x > 1.5) { s += x; x = 1.0; } }
  return s + x;
}
static double p_cvt(void) { // f64 <-> i32/i64 truncation and back
  double x = vd, s = 0;
  for (int i = 0; i < N; i++) { int32_t k = (int32_t)(x * 100.0); int64_t q = (int64_t)(x * 1e6); s += k + (double)q * 1e-6; x += 1e-7; }
  return s;
}
static double p_u64f(void) { // u64 -> f64 (the SSE2 punpckldq/subpd sequence) and f64 -> u64
  uint64_t u = vu; double s = 0;
  for (int i = 0; i < N; i++) { s += (double)u; u += (uint64_t)(s * 1e-12) + 7; }
  return s;
}
static double p_fdiv(void) { double x = vd, s = 0; for (int i = 0; i < N; i++) { s += 1.0 / (x + i); } return s; }
static double p_fsqrt(void) { double s = 0; for (int i = 0; i < N; i++) s += __builtin_sqrt((double)i); return s; }
static double p_minmax(void) { // f64 min/max/clamp (Rust's clamp, f64::max)
  double x = vd, s = 0;
  for (int i = 0; i < N; i++) { double y = x * 1.1 - 0.05; y = y < 0.0 ? 0.0 : y > 1.0 ? 1.0 : y; s += __builtin_fmax(y, 0.25); x = y + 1e-3; }
  return s;
}
static double p_setcc(void) { // integer compare to bool, cmov, flags-heavy code
  uint64_t a = vu, b = 0; uint32_t c = 0;
  for (int i = 0; i < N; i++) { c += (a > b) + (a == (uint64_t)i); b = (a & 1) ? b + a : b ^ a; a = a * 6364136223846793005ULL + 1; }
  return (double)c + (double)b;
}
static double p_mem(void) { // stores and loads through a base pointer, like wasm2c's linear memory
  static uint8_t m[1 << 16]; uint32_t idx = 0; double s = 0;
  for (int i = 0; i < N; i++) { double v; memcpy(&v, m + idx, 8); v += 1.0; memcpy(m + idx, &v, 8); s += v; idx = (idx + 72) & 0xfff8; }
  return s;
}
static double p_div64(void) { uint64_t a = vu, s = 0; for (int i = 1; i < N; i++) { s += a / (uint64_t)i + a % 7; a += s; } return (double)s; }

static struct { const char *n; double (*f)(void); } P[] = {
  {"call", p_call}, {"saver", p_saver}, {"icall", p_icall}, {"fcmp", p_fcmp}, {"cvt", p_cvt}, {"u64f", p_u64f},
  {"fdiv", p_fdiv}, {"fsqrt", p_fsqrt}, {"minmax", p_minmax}, {"setcc", p_setcc}, {"mem", p_mem}, {"div64", p_div64},
};

int main(int argc, char **argv) {
  for (unsigned k = 0; k < sizeof P / sizeof P[0]; k++) {
    int want = argc < 2;
    for (int a = 1; a < argc; a++) want |= !strcmp(argv[a], P[k].n);
    if (!want) continue;
    double best = 1e30, v = 0;
    for (int r = 0; r < 5; r++) {
      double t0 = now_ns();
      v = P[k].f();
      double t = (now_ns() - t0) / N;
      if (t < best) best = t;
    }
    printf("RESULT %-7s ns=%7.3f value=%g\n", P[k].n, best, v);
    fflush(stdout);
  }
  return 0;
}
