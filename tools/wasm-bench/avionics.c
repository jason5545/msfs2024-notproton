// WebAssembly guest: avionics-style work like a big MSFS WASM gauge (systems, FMS, displays) runs every
// frame. Compiled to wasm32 with no libc (build.sh), then wasm2c turns it back into C that the host side
// compiles for x64 the way an AOT WASM compiler would. Each k_* kernel is timed on its own by host.c.
// Every kernel returns a value the host prints so nothing is optimised away.
#include <stdint.h>

#define EXPORT(n) __attribute__((export_name(n)))
typedef unsigned long size_t;

static double fabs_(double x) { return __builtin_fabs(x); }
static double sqrt_(double x) { return __builtin_sqrt(x); }
static double floor_(double x) { return __builtin_floor(x); }

// Polynomial sin/cos/atan like a gauge's own math library (no libm in the module).
static double sin_(double x) {
  double k = floor_(x * 0.15915494309189535 + 0.5);
  x -= k * 6.283185307179586;
  double x2 = x * x;
  return x * (1 + x2 * (-1.0 / 6 + x2 * (1.0 / 120 + x2 * (-1.0 / 5040 + x2 * (1.0 / 362880)))));
}
static double cos_(double x) { return sin_(x + 1.5707963267948966); }
static double atan_(double x) {
  int inv = fabs_(x) > 1;
  if (inv) x = 1 / x;
  double x2 = x * x;
  double r = x * (0.9998660 + x2 * (-0.3302995 + x2 * (0.1801410 + x2 * (-0.0851330 + x2 * 0.0208351))));
  if (inv) r = (x > 0 ? 1.5707963267948966 : -1.5707963267948966) - r;
  return r;
}
static double atan2_(double y, double x) {
  if (x > 0) return atan_(y / x);
  if (x < 0) return atan_(y / x) + (y >= 0 ? 3.141592653589793 : -3.141592653589793);
  return y > 0 ? 1.5707963267948966 : -1.5707963267948966;
}

#define N 4096
static double A[N], B[N], C[N];
static float F[N], G[N];
static uint32_t W[N];  // ARINC 429 style words
static int32_t I[N];

EXPORT("k_init") void k_init(void) {
  uint32_t s = 12345;
  for (int i = 0; i < N; i++) {
    s = s * 1664525 + 1013904223;
    A[i] = (s >> 8) * (1.0 / 16777216) - 0.5;
    B[i] = (double)i / N;
    C[i] = 0;
    F[i] = (float)A[i];
    G[i] = (float)B[i];
    W[i] = s;
    I[i] = (int32_t)(s % N);
  }
}

// 1. vector math the compiler can vectorise (axpy, dot, norm): 256-bit lanes with AVX
EXPORT("k_vec") double k_vec(int reps) {
  double acc = 0;
  for (int r = 0; r < reps; r++) {
    double a = 0.5 + r * 1e-6;
    for (int i = 0; i < N; i++) C[i] = a * A[i] + B[i];
    double d = 0;
    for (int i = 0; i < N; i++) d += C[i] * A[i];
    acc += d;
  }
  return acc;
}

// 2. 3x3 rotation of many points (attitude, ND symbols): small fixed loops, shuffles with AVX
typedef struct { double x, y, z; } v3;
static v3 P[N / 4], Q[N / 4];
EXPORT("k_mat") double k_mat(int reps) {
  double acc = 0;
  for (int r = 0; r < reps; r++) {
    double h = r * 0.001, p = 0.05, b = 0.02;
    double ch = cos_(h), sh = sin_(h), cp = cos_(p), sp = sin_(p), cb = cos_(b), sb = sin_(b);
    double m[3][3] = {{ch * cp, ch * sp * sb - sh * cb, ch * sp * cb + sh * sb},
                      {sh * cp, sh * sp * sb + ch * cb, sh * sp * cb - ch * sb},
                      {-sp, cp * sb, cp * cb}};
    for (int i = 0; i < N / 4; i++) {
      v3 v = {A[i], B[i], A[i + 1]};
      Q[i].x = m[0][0] * v.x + m[0][1] * v.y + m[0][2] * v.z;
      Q[i].y = m[1][0] * v.x + m[1][1] * v.y + m[1][2] * v.z;
      Q[i].z = m[2][0] * v.x + m[2][1] * v.y + m[2][2] * v.z;
    }
    acc += Q[r % (N / 4)].x;
  }
  return acc;
}

// 3. table lookup with interpolation (engine maps, aero tables): indexed loads, gathers with AVX2
#define T 256
static double TAB[T + 1], TAB2[T + 1][T + 1];
EXPORT("k_interp") double k_interp(int reps) {
  for (int i = 0; i <= T; i++) {
    TAB[i] = sin_(i * 0.01);
    for (int j = 0; j <= T; j++) TAB2[i][j] = i * 0.5 + j * 0.25;
  }
  double acc = 0;
  for (int r = 0; r < reps; r++) {
    for (int i = 0; i < N; i++) {
      double x = (A[i] + 0.5) * (T - 1);
      int k = (int)x;
      double f = x - k;
      C[i] = TAB[k] + f * (TAB[k + 1] - TAB[k]);
    }
    for (int i = 0; i < N; i += 4) {
      double x = (A[i] + 0.5) * (T - 1), y = B[i] * (T - 1);
      int ix = (int)x, iy = (int)y;
      double fx = x - ix, fy = y - iy;
      double v0 = TAB2[ix][iy] + fx * (TAB2[ix + 1][iy] - TAB2[ix][iy]);
      double v1 = TAB2[ix][iy + 1] + fx * (TAB2[ix + 1][iy + 1] - TAB2[ix][iy + 1]);
      acc += v0 + fy * (v1 - v0);
    }
    acc += C[r & (N - 1)];
  }
  return acc;
}

// 4. virtual dispatch over system objects (call_indirect through the table)
typedef struct Sys Sys;
struct Sys { double (*update)(Sys *, double); double state, k; };
__attribute__((noinline)) static double upd_a(Sys *s, double dt) { s->state += dt * s->k; return s->state; }
__attribute__((noinline)) static double upd_b(Sys *s, double dt) { s->state = s->state * 0.99 + dt; return s->state; }
__attribute__((noinline)) static double upd_c(Sys *s, double dt) { s->state = sqrt_(fabs_(s->state) + dt); return s->state; }
__attribute__((noinline)) static double upd_d(Sys *s, double dt) { s->state -= s->k * s->state * dt; return s->state; }
static Sys S[512];
EXPORT("k_virt") double k_virt(int reps) {
  double (*fns[4])(Sys *, double) = {upd_a, upd_b, upd_c, upd_d};
  for (int i = 0; i < 512; i++) { S[i].update = fns[W[i] & 3]; S[i].state = A[i]; S[i].k = B[i]; }
  double acc = 0;
  for (int r = 0; r < reps; r++)
    for (int i = 0; i < 512; i++) acc += S[i].update(&S[i], 0.016);
  return acc;
}

// 5. bus words: label, SSM, sign-extended data fields, parity (shifts, masks, popcount)
EXPORT("k_int") int32_t k_int(int reps) {
  uint32_t acc = 0;
  for (int r = 0; r < reps; r++)
    for (int i = 0; i < N; i++) {
      uint32_t w = W[i] ^ (uint32_t)r;
      uint32_t label = w & 0xff, ssm = (w >> 29) & 3;
      int32_t data = (int32_t)(w << 3) >> 13;
      uint32_t par = __builtin_popcount(w) & 1;
      acc += (label * 31 + ssm) ^ (uint32_t)data ^ par;
      acc = (acc << 5) | (acc >> 27);
    }
  return (int32_t)acc;
}

// 6. state blocks copied between frames (double buffering, memcpy/memset)
typedef struct { double v[48]; int32_t flags[16]; } Block;
static Block BA[64], BB[64];
EXPORT("k_copy") double k_copy(int reps) {
  double acc = 0;
  for (int r = 0; r < reps; r++) {
    for (int i = 0; i < 64; i++) BB[i] = BA[(i + r) & 63];
    BA[r & 63].v[r % 48] += 1;
    acc += BB[r & 63].v[0];
  }
  return acc;
}

// 7. display-side float work with float/double/int conversions and rounding (pixel snapping)
EXPORT("k_f32") double k_f32(int reps) {
  double acc = 0;
  for (int r = 0; r < reps; r++)
    for (int i = 0; i < N; i++) {
      float x = F[i] * 512.0f + G[i] * 3.0f;
      int px = (int)__builtin_rintf(x);
      double d = (double)x - px;
      I[i] = px;
      acc += d;
    }
  return acc;
}

// 8. navigation: great-circle distance and bearing (trig, sqrt, atan2, branches)
EXPORT("k_nav") double k_nav(int reps) {
  double acc = 0;
  for (int r = 0; r < reps; r++)
    for (int i = 0; i < 1024; i++) {
      double la1 = A[i], lo1 = B[i], la2 = A[i + 1], lo2 = B[i + 1] + r * 1e-4;
      double dla = la2 - la1, dlo = lo2 - lo1;
      double s1 = sin_(dla * 0.5), s2 = sin_(dlo * 0.5);
      double a = s1 * s1 + cos_(la1) * cos_(la2) * s2 * s2;
      double dist = 2 * atan2_(sqrt_(a), sqrt_(1 - a));
      double brg = atan2_(sin_(dlo) * cos_(la2), cos_(la1) * sin_(la2) - sin_(la1) * cos_(la2) * cos_(dlo));
      acc += dist + brg;
    }
  return acc;
}
