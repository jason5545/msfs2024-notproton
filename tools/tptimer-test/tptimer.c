// tptimer.exe: thread pool timers (CreateThreadpoolTimer / SetThreadpoolTimer) in the shapes that decide how
// Wine's timer queue thread (wine_threadpool_timerqueue, dlls/ntdll/threadpool.c) waits.
//
//   tptimer.exe <mode> [seconds=5] [ms] [timers=1]
//     none     no thread pool timer at all (the timer queue thread is never started)
//     idle     timers created, never set
//     abs      one-shot, absolute due time now + ms (default 3600000: never fires in the run)
//     rel      one-shot, relative due time ms (default 3600000)
//     per      relative due time ms, period ms (default 16)
//     absper   absolute due time now + ms, period ms (default 16)
//   "timers" makes that many timers of the same kind (each with its own counters; stats are over all).
//
// Prints "READY" once the timers are set, then after <seconds>: callbacks, intervals between callbacks of
// the same timer (ms), the first callback's lateness against its due time, and the process's CPU time
// (GetProcessTimes) over the run. Per-thread CPU and wait counts come from Wine patch 0061 (the wait
// recorder), see run.sh.
//
// Build: build.sh (x86_64-w64-mingw32-clang from llvm-mingw); runs on FEX under Wine.
#define _WIN32_WINNT 0x0602
#include <windows.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define MAX_TIMERS 64
#define MAX_STAMPS 200000

struct tstate {
  PTP_TIMER timer;
  double set_ms, due_ms;           /* when it was set, and when the first callback was due (qpc ms) */
  volatile LONG count;
  double *stamps;                  /* qpc ms of each callback (first MAX_STAMPS) */
};

static struct tstate ts[MAX_TIMERS];

static double qpc_ms(void)
{
  static LARGE_INTEGER f; LARGE_INTEGER c;
  if (!f.QuadPart) QueryPerformanceFrequency(&f);
  QueryPerformanceCounter(&c);
  return c.QuadPart * 1000.0 / f.QuadPart;
}

static double ft_ms(FILETIME a) { return ((ULONGLONG)a.dwHighDateTime << 32 | a.dwLowDateTime) / 10000.0; }

static void CALLBACK cb(PTP_CALLBACK_INSTANCE inst, void *ctx, PTP_TIMER timer)
{
  struct tstate *t = ctx;
  double now = qpc_ms();
  LONG n = InterlockedIncrement(&t->count) - 1;
  if (n < MAX_STAMPS) t->stamps[n] = now;
  (void)inst; (void)timer;
}

static int cmp(const void *a, const void *b)
{
  double x = *(const double *)a, y = *(const double *)b;
  return x < y ? -1 : x > y;
}

int main(int argc, char **argv)
{
  const char *mode = argc > 1 ? argv[1] : "idle";
  int seconds = argc > 2 ? atoi(argv[2]) : 5;
  int periodic = !strcmp(mode, "per") || !strcmp(mode, "absper");
  DWORD ms = argc > 3 ? (DWORD)atol(argv[3]) : (periodic ? 16 : 3600000);
  int ntimers = argc > 4 ? atoi(argv[4]) : 1;
  int absolute = !strcmp(mode, "abs") || !strcmp(mode, "absper");
  FILETIME c0, e0, k0, u0, k1, u1;
  HANDLE idle_ev = CreateEventW(NULL, TRUE, FALSE, NULL);
  double t_start, t_end;
  int i;

  if (ntimers < 1) ntimers = 1;
  if (ntimers > MAX_TIMERS) ntimers = MAX_TIMERS;
  setvbuf(stdout, NULL, _IONBF, 0);
  if (strcmp(mode, "none") && strcmp(mode, "idle") && strcmp(mode, "abs") && strcmp(mode, "rel")
      && !periodic)
  {
    fprintf(stderr, "unknown mode %s\n", mode);
    return 2;
  }

  if (strcmp(mode, "none"))
  {
    for (i = 0; i < ntimers; i++)
    {
      ts[i].stamps = calloc(MAX_STAMPS, sizeof(double));
      ts[i].timer = CreateThreadpoolTimer(cb, &ts[i], NULL);
      if (!ts[i].timer) { fprintf(stderr, "CreateThreadpoolTimer failed %lu\n", GetLastError()); return 1; }
    }
  }
  for (i = 0; i < ntimers && strcmp(mode, "none") && strcmp(mode, "idle"); i++)
  {
    FILETIME due;
    ULARGE_INTEGER u;
    if (absolute)
    {
      GetSystemTimeAsFileTime(&due);
      u.LowPart = due.dwLowDateTime; u.HighPart = due.dwHighDateTime;
      u.QuadPart += (ULONGLONG)ms * 10000;
    }
    else u.QuadPart = (ULONGLONG)(-(LONGLONG)ms * 10000);
    due.dwLowDateTime = u.LowPart; due.dwHighDateTime = u.HighPart;
    ts[i].set_ms = qpc_ms();
    ts[i].due_ms = ts[i].set_ms + ms;
    SetThreadpoolTimer(ts[i].timer, &due, periodic ? ms : 0, 0);
  }

  GetProcessTimes(GetCurrentProcess(), &c0, &e0, &k0, &u0);
  t_start = qpc_ms();
  printf("READY mode=%s seconds=%d ms=%lu timers=%d\n", mode, seconds, (unsigned long)ms, ntimers);
  WaitForSingleObject(idle_ev, seconds * 1000);
  t_end = qpc_ms();
  GetProcessTimes(GetCurrentProcess(), &c0, &e0, &k1, &u1);

  {
    LONG total = 0;
    size_t nint = 0, k;
    double *iv = malloc(sizeof(double) * MAX_STAMPS * (size_t)ntimers), late_sum = 0, late_max = -1e9;
    int nlate = 0;
    for (i = 0; i < ntimers && strcmp(mode, "none"); i++)
    {
      LONG n = ts[i].count < MAX_STAMPS ? ts[i].count : MAX_STAMPS;
      total += ts[i].count;
      if (n > 0 && ts[i].due_ms > 0)
      {
        double late = ts[i].stamps[0] - ts[i].due_ms;
        late_sum += late; nlate++;
        if (late > late_max) late_max = late;
      }
      for (k = 1; k < (size_t)n; k++) iv[nint++] = ts[i].stamps[k] - ts[i].stamps[k - 1];
    }
    printf("mode=%s run_ms=%.1f callbacks=%ld (%.1f/s)\n", mode, t_end - t_start, (long)total,
           total * 1000.0 / (t_end - t_start));
    printf("process cpu_ms user=%.1f kernel=%.1f total=%.1f (%.1f%% of one core)\n",
           ft_ms(u1) - ft_ms(u0), ft_ms(k1) - ft_ms(k0), ft_ms(u1) - ft_ms(u0) + ft_ms(k1) - ft_ms(k0),
           (ft_ms(u1) - ft_ms(u0) + ft_ms(k1) - ft_ms(k0)) * 100.0 / (t_end - t_start));
    if (nlate)
      printf("first callback late_ms mean=%.3f max=%.3f (n=%d)\n", late_sum / nlate, late_max, nlate);
    if (nint)
    {
      double sum = 0;
      qsort(iv, nint, sizeof(double), cmp);
      for (k = 0; k < nint; k++) sum += iv[k];
      printf("interval_ms n=%zu mean=%.3f min=%.3f p50=%.3f p95=%.3f p99=%.3f max=%.3f\n", nint,
             sum / nint, iv[0], iv[nint / 2], iv[nint * 95 / 100], iv[nint * 99 / 100], iv[nint - 1]);
    }
  }

  for (i = 0; i < ntimers && strcmp(mode, "none"); i++)
  {
    SetThreadpoolTimer(ts[i].timer, NULL, 0, 0);
    WaitForThreadpoolTimerCallbacks(ts[i].timer, TRUE);
    CloseThreadpoolTimer(ts[i].timer);
  }
  /* a wait after the recording window, so that the recorder gathers names and writes the file in "none" too */
  WaitForSingleObject(idle_ev, 300);
  return 0;
}
