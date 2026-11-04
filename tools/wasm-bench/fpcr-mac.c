// Native arm64 macOS: what reading and writing FPCR costs on this core (FEX's ARM64EC dispatcher reads it
// three times every time x64 code comes back from an ARM64EC call). ns per operation, best of 5.
#include <stdint.h>
#include <stdio.h>
#include <time.h>

#define N 20000000
static double now_ns(void) {
  struct timespec t;
  clock_gettime(CLOCK_MONOTONIC, &t);
  return t.tv_sec * 1e9 + t.tv_nsec;
}
static inline uint64_t rd(void) { uint64_t v; __asm__ volatile("mrs %0, fpcr" : "=r"(v)); return v; }
static inline void wr(uint64_t v) { __asm__ volatile("msr fpcr, %0" ::"r"(v)); }

int main(void) {
  uint64_t base = rd(), acc = 0;
  const char *names[] = {"empty loop", "mrs fpcr", "msr fpcr (same value)", "msr fpcr (toggle NEP|AH)",
                         "mrs+msr toggle (exit+entry)", "mrs fpsr"};
  for (int k = 0; k < 6; k++) {
    double best = 1e30;
    for (int r = 0; r < 5; r++) {
      double t0 = now_ns();
      for (int i = 0; i < N; i++) {
        switch (k) {
        case 0: __asm__ volatile("" ::: "memory"); break;
        case 1: acc += rd(); break;
        case 2: wr(base); break;
        case 3: wr((i & 1) ? base | 6 : base); break;
        case 4: { uint64_t v = rd(); wr(v | 6); v = rd(); wr(v & ~6ull); } break;
        case 5: { uint64_t v; __asm__ volatile("mrs %0, fpsr" : "=r"(v)); acc += v; } break;
        }
      }
      double t = (now_ns() - t0) / N;
      if (t < best) best = t;
    }
    printf("RESULT %-28s ns=%.2f\n", names[k], best);
  }
  wr(base);
  return (int)(acc & 1);
}
