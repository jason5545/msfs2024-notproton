// Host for the wasm2c output of avionics.wasm: instantiates the module and times each kernel
// (best of ROUNDS, in ms). Same source for the x64 PE under Wine/FEX and the arm64 macOS reference.
//
//   wasmbench.exe [kernel...]     default: all kernels
#include <stdio.h>
#include <string.h>
#include "avionics.h"

#ifdef _WIN32
#include <windows.h>
static double now_ms(void) {
  static LARGE_INTEGER f;
  LARGE_INTEGER c;
  if (!f.QuadPart) QueryPerformanceFrequency(&f);
  QueryPerformanceCounter(&c);
  return c.QuadPart * 1000.0 / f.QuadPart;
}
#else
#include <time.h>
static double now_ms(void) {
  struct timespec t;
  clock_gettime(CLOCK_MONOTONIC, &t);
  return t.tv_sec * 1000.0 + t.tv_nsec / 1e6;
}
#endif

#define ROUNDS 5
static w2c_avionics inst;

#define K(name, reps, type, fmt)                                                   \
  if (want(argc, argv, #name)) {                                                   \
    double best = 1e30;                                                            \
    type v = 0;                                                                    \
    for (int r = 0; r < ROUNDS; r++) {                                             \
      double t0 = now_ms();                                                        \
      v = w2c_avionics_##name(&inst, reps);                                        \
      double t = now_ms() - t0;                                                    \
      if (t < best) best = t;                                                      \
    }                                                                              \
    printf("RESULT %-9s ms=%9.3f value=" fmt "\n", #name, best, v);                \
    total += best;                                                                 \
  }

static int want(int argc, char **argv, const char *n) {
  if (argc < 2) return 1;
  for (int i = 1; i < argc; i++)
    if (!strcmp(argv[i], n)) return 1;
  return 0;
}

int main(int argc, char **argv) {
  wasm_rt_init();
  wasm2c_avionics_instantiate(&inst);
  w2c_avionics_k_init(&inst);
  double total = 0;
  K(k_vec, 2000, f64, "%.6g")
  K(k_mat, 400, f64, "%.6g")
  K(k_interp, 1000, f64, "%.6g")
  K(k_virt, 4000, f64, "%.6g")
  K(k_int, 2000, u32, "%u")
  K(k_copy, 40000, f64, "%.6g")
  K(k_f32, 2000, f64, "%.6g")
  K(k_nav, 400, f64, "%.6g")
  printf("SUMMARY total_ms=%.3f\n", total);
  fflush(stdout);
  wasm2c_avionics_free(&inst);
  wasm_rt_free();
  return 0;
}
