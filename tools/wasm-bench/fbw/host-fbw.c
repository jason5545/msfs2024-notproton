// Host of the FBW A380X systems benchmark (fbw/lib.rs through wasm2c): builds the aircraft, then times
// simulation ticks of 33 ms (a 30 fps frame): the first FIRST ticks, WARM_MS of warm-up, and FRAMES
// measured ticks (median and max, ms per tick). HOLD=<s> waits at the warm/done points for fexstats.py.
#include <stdio.h>
#include <stdlib.h>
#include "fbw.h"

#ifdef _WIN32
#include <windows.h>
static double now_ms(void) {
  static LARGE_INTEGER f;
  LARGE_INTEGER c;
  if (!f.QuadPart) QueryPerformanceFrequency(&f);
  QueryPerformanceCounter(&c);
  return c.QuadPart * 1000.0 / f.QuadPart;
}
static void hold(int s) { Sleep(s * 1000); }
#else
#include <time.h>
#include <unistd.h>
static double now_ms(void) {
  struct timespec t;
  clock_gettime(CLOCK_MONOTONIC, &t);
  return t.tv_sec * 1000.0 + t.tv_nsec / 1e6;
}
static void hold(int s) { sleep(s); }
#endif

#define FIRST 30
#define FRAMES 600
static w2c_fbw inst;
static int cmp(const void *a, const void *b) { double x = *(double *)a, y = *(double *)b; return x < y ? -1 : x > y; }

int main(void) {
  const char *h = getenv("HOLD"), *w = getenv("WARM_MS");
  int hs = h ? atoi(h) : 0;
  double warm = w ? atof(w) : 6000;
  wasm_rt_init();
  wasm2c_fbw_instantiate(&inst);
  double t0 = now_ms();
  w2c_fbw_bench_init(&inst);
  double tinit = now_ms() - t0;
  t0 = now_ms();
  w2c_fbw_bench_run(&inst, FIRST, 33);
  double tfirst = now_ms() - t0;
  double ws = now_ms();
  int warmframes = 0;
  while (now_ms() - ws < warm) { w2c_fbw_bench_run(&inst, 1, 33); warmframes++; }
  printf("PHASE warm frames=%d\n", warmframes);
  fflush(stdout);
  if (hs) hold(hs);
  static double t[FRAMES];
  double v = 0;
  for (int i = 0; i < FRAMES; i++) {
    t0 = now_ms();
    v = w2c_fbw_bench_run(&inst, 1, 33);
    t[i] = now_ms() - t0;
  }
  qsort(t, FRAMES, sizeof t[0], cmp);
  printf("RESULT init_ms=%.2f first%d_ms=%.2f tick_median_ms=%.4f tick_p90_ms=%.4f tick_max_ms=%.4f green_psi=%.1f\n",
         tinit, FIRST, tfirst, t[FRAMES / 2], t[FRAMES * 9 / 10], t[FRAMES - 1], v);
  printf("PHASE done\n");
  fflush(stdout);
  if (hs) hold(hs);
  return 0;
}
