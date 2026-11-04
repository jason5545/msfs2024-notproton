// Host of the code-footprint benchmark (gen-big.py, build-big.sh): frame 1 (translation), frame 2, and
// the median of FRAMES steady frames, in ms. With HOLD=<s> it waits that long after the warm-up frames
// and before it exits, so tools/wasm-bench/fexstats.py can read FEX's stats of a running process.
#include <stdio.h>
#include <stdlib.h>
#include "big.h"

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

#define FRAMES 200
static w2c_big inst;
static int cmp(const void *a, const void *b) { double x = *(double *)a, y = *(double *)b; return x < y ? -1 : x > y; }

int main(void) {
  const char *h = getenv("HOLD");
  int hs = h ? atoi(h) : 0;
  wasm_rt_init();
  wasm2c_big_instantiate(&inst);
  w2c_big_init(&inst);
  double t0 = now_ms();
  w2c_big_frame(&inst, 1);
  double f1 = now_ms() - t0;
  t0 = now_ms();
  w2c_big_frame(&inst, 1);
  double f2 = now_ms() - t0;
  // WARM_MS (default 6000): keep running frames that long first, so FEX's dynamic L1 lookup cache
  // (resized at most once a second on a miss) settles the way it would in a game
  const char *w = getenv("WARM_MS");
  double warm = w ? atof(w) : 6000, ws = now_ms();
  int warmframes = 0;
  while (now_ms() - ws < warm || warmframes < 5) { w2c_big_frame(&inst, 1); warmframes++; }
  printf("PHASE warm frames=%d\n", warmframes);
  fflush(stdout);
  if (hs) hold(hs);
  double t[FRAMES], v = 0;
  for (int i = 0; i < FRAMES; i++) {
    t0 = now_ms();
    v += w2c_big_frame(&inst, 1);
    t[i] = now_ms() - t0;
  }
  qsort(t, FRAMES, sizeof t[0], cmp);
  printf("RESULT frame1_ms=%.2f frame2_ms=%.2f steady_median_ms=%.3f steady_max_ms=%.3f value=%g\n", f1, f2,
         t[FRAMES / 2], t[FRAMES - 1], v);
  printf("PHASE done\n");
  fflush(stdout);
  if (hs) hold(hs);
  return 0;
}
