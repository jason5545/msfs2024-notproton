// waittest.exe: threads whose waits are known in advance, for Wine patch 0061 (the wait recorder).
//
//   waittest.exe roles [seconds=8] [period_ms=10]
//     A  waits on an auto-reset event that B sets every period_ms (server path, woken by B with set_event)
//     B  Sleep(period_ms), SetEvent(A's event), and every 4th round WakeConditionVariable for V
//     C1/C2  fight over one critical section, holding it ~0.5 ms each time (alert path, woken by each other)
//     S1/S2  the same with an exclusive SRW lock
//     V  SleepConditionVariableSRW until B wakes it (alert path, condition variable, woken by B)
//     Y  spins with SwitchToThread() until B bumps a counter (yields, no wait)
//     P  pure spin: no call at all (must show as busy, never as a wait)
//   Prints "ROLE <name> tid=<n>" for each thread, "READY" once all run, counts at the end.
//
//   waittest.exe bench [rounds=20000] [delay_ms=0] [hold_ms=0]
//     per-operation cost of each wait path, for the recorder's overhead: event ping-pong (server),
//     WaitOnAddress ping-pong (alert), uncontended critical section, Sleep(0), zero-timeout wait.
//
// Build: x86_64-w64-mingw32-clang -O2 -o waittest.exe waittest.c (llvm-mingw); runs on FEX under Wine.
#define _WIN32_WINNT 0x0602
#include <windows.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static volatile LONG stop;
static HANDLE ev_a;
static CRITICAL_SECTION cs;
static SRWLOCK srw = SRWLOCK_INIT, cv_lock = SRWLOCK_INIT;
static CONDITION_VARIABLE cv = CONDITION_VARIABLE_INIT;
static volatile LONG cv_gen, y_gen;
static DWORD period_ms = 10;
static volatile LONG ready;

struct role { const char *name; LPTHREAD_START_ROUTINE fn; DWORD tid; volatile LONG64 count; };

static double qpc_ms(void)
{
  static LARGE_INTEGER f; LARGE_INTEGER c;
  if (!f.QuadPart) QueryPerformanceFrequency(&f);
  QueryPerformanceCounter(&c);
  return c.QuadPart * 1000.0 / f.QuadPart;
}

static void busy_us(double us)
{
  double end = qpc_ms() + us / 1000.0;
  volatile unsigned x = 1;
  while (qpc_ms() < end) for (int i = 0; i < 200; i++) x = x * 1664525u + 1013904223u;
}

static struct role roles[];
#define R(i) (&roles[i])

static void hello(struct role *r)
{
  typedef HRESULT (WINAPI *set_desc_fn)(HANDLE, PCWSTR);
  set_desc_fn set_desc = (set_desc_fn)GetProcAddress(GetModuleHandleW(L"kernelbase.dll"), "SetThreadDescription");
  WCHAR name[32];
  swprintf(name, 32, L"waittest-%hs", r->name);
  if (set_desc) set_desc(GetCurrentThread(), name);
  r->tid = GetCurrentThreadId();
  printf("ROLE %s tid=%lu\n", r->name, r->tid);
  fflush(stdout);
  InterlockedIncrement(&ready);
}

static DWORD WINAPI role_a(void *p)
{
  struct role *r = p; hello(r);
  while (!stop) if (WaitForSingleObject(ev_a, 1000) == WAIT_OBJECT_0) r->count++;
  return 0;
}

static DWORD WINAPI role_b(void *p)
{
  struct role *r = p; hello(r);
  while (!stop)
  {
    Sleep(period_ms);
    SetEvent(ev_a);
    InterlockedIncrement(&y_gen);
    if (!(r->count % 4))
    {
      AcquireSRWLockExclusive(&cv_lock);
      cv_gen++;
      ReleaseSRWLockExclusive(&cv_lock);
      WakeConditionVariable(&cv);
    }
    r->count++;
  }
  SetEvent(ev_a);
  WakeAllConditionVariable(&cv);
  return 0;
}

static DWORD WINAPI role_c(void *p)
{
  struct role *r = p; hello(r);
  while (!stop)
  {
    EnterCriticalSection(&cs);
    busy_us(500);
    r->count++;
    LeaveCriticalSection(&cs);
  }
  return 0;
}

static DWORD WINAPI role_s(void *p)
{
  struct role *r = p; hello(r);
  while (!stop)
  {
    AcquireSRWLockExclusive(&srw);
    busy_us(500);
    r->count++;
    ReleaseSRWLockExclusive(&srw);
  }
  return 0;
}

static DWORD WINAPI role_v(void *p)
{
  struct role *r = p; hello(r);
  AcquireSRWLockExclusive(&cv_lock);
  while (!stop)
  {
    LONG seen = cv_gen;
    while (cv_gen == seen && !stop) SleepConditionVariableSRW(&cv, &cv_lock, 1000, 0);
    r->count++;
  }
  ReleaseSRWLockExclusive(&cv_lock);
  return 0;
}

static DWORD WINAPI role_y(void *p)
{
  struct role *r = p; hello(r);
  while (!stop)
  {
    LONG seen = y_gen;
    while (y_gen == seen && !stop) SwitchToThread();
    r->count++;
  }
  return 0;
}

static DWORD WINAPI role_p(void *p)
{
  struct role *r = p; hello(r);
  volatile unsigned x = 1;
  while (!stop) { for (int i = 0; i < 100000; i++) x = x * 1664525u + 1013904223u; r->count++; }
  return 0;
}

static struct role roles[] = {
  {"A", role_a}, {"B", role_b}, {"C1", role_c}, {"C2", role_c}, {"S1", role_s}, {"S2", role_s},
  {"V", role_v}, {"Y", role_y}, {"P", role_p},
};

static int run_roles(double seconds)
{
  HANDLE th[16];
  int n = sizeof(roles) / sizeof(roles[0]);
  ev_a = CreateEventW(NULL, FALSE, FALSE, NULL);
  InitializeCriticalSection(&cs);
  printf("MAIN tid=%lu period_ms=%lu seconds=%.1f\n", GetCurrentThreadId(), period_ms, seconds);
  printf("OBJ event_a=%p cs=%p cs_lock=%p srw=%p cv=%p\n", ev_a, (void *)&cs, (void *)&cs.LockSemaphore,
         (void *)&srw, (void *)&cv);
  for (int i = 0; i < n; i++) th[i] = CreateThread(NULL, 0, roles[i].fn, &roles[i], 0, NULL);
  while (ready < n) Sleep(1);
  printf("READY %.3f\n", qpc_ms()); fflush(stdout);
  double t0 = qpc_ms();
  // the main thread waits on the threads it made, for the whole run (one long server wait)
  WaitForMultipleObjects(1, &th[0], FALSE, (DWORD)(seconds * 1000));
  stop = 1;
  WaitForMultipleObjects(n, th, TRUE, INFINITE);
  double t1 = qpc_ms();
  for (int i = 0; i < n; i++) printf("COUNT %s %lld\n", roles[i].name, (long long)roles[i].count);
  printf("DONE %.3f s\n", (t1 - t0) / 1000.0);
  return 0;
}

// --- bench

static HANDLE pp_ev[2];
static volatile LONG turn;
static LONG rounds = 20000;

static DWORD WINAPI pp_event_peer(void *p)
{
  for (LONG i = 0; i < rounds; i++) { WaitForSingleObject(pp_ev[0], INFINITE); SetEvent(pp_ev[1]); }
  return 0;
}

static DWORD WINAPI pp_addr_peer(void *p)
{
  for (LONG i = 0; i < rounds; i++)
  {
    LONG one = 1;
    while (turn != 1) WaitOnAddress(&turn, &(LONG){0}, sizeof(LONG), INFINITE);
    turn = 0; WakeByAddressSingle((void *)&turn);
    (void)one;
  }
  return 0;
}

static void bench(void)
{
  double t; HANDLE h;
  pp_ev[0] = CreateEventW(NULL, FALSE, FALSE, NULL); pp_ev[1] = CreateEventW(NULL, FALSE, FALSE, NULL);
  h = CreateThread(NULL, 0, pp_event_peer, NULL, 0, NULL);
  t = qpc_ms();
  for (LONG i = 0; i < rounds; i++) { SetEvent(pp_ev[0]); WaitForSingleObject(pp_ev[1], INFINITE); }
  t = qpc_ms() - t; WaitForSingleObject(h, INFINITE); CloseHandle(h);
  printf("BENCH event_pingpong rounds=%ld us_per_round=%.3f\n", rounds, t * 1000.0 / rounds);

  turn = 0;
  h = CreateThread(NULL, 0, pp_addr_peer, NULL, 0, NULL);
  t = qpc_ms();
  for (LONG i = 0; i < rounds; i++)
  {
    turn = 1; WakeByAddressSingle((void *)&turn);
    while (turn != 0) WaitOnAddress(&turn, &(LONG){1}, sizeof(LONG), INFINITE);
  }
  t = qpc_ms() - t; WaitForSingleObject(h, INFINITE); CloseHandle(h);
  printf("BENCH address_pingpong rounds=%ld us_per_round=%.3f\n", rounds, t * 1000.0 / rounds);

  CRITICAL_SECTION c; InitializeCriticalSection(&c);
  LONG n = rounds * 100;
  t = qpc_ms();
  for (LONG i = 0; i < n; i++) { EnterCriticalSection(&c); LeaveCriticalSection(&c); }
  t = qpc_ms() - t;
  printf("BENCH critsec_uncontended n=%ld ns_per_op=%.2f\n", n, t * 1e6 / n);

  n = rounds;
  t = qpc_ms();
  for (LONG i = 0; i < n; i++) Sleep(0);
  t = qpc_ms() - t;
  printf("BENCH sleep0 n=%ld us_per_op=%.3f\n", n, t * 1000.0 / n);

  HANDLE m = CreateEventW(NULL, TRUE, TRUE, NULL);
  t = qpc_ms();
  for (LONG i = 0; i < n; i++) WaitForSingleObject(m, 0);
  t = qpc_ms() - t;
  printf("BENCH wait_signaled_poll n=%ld us_per_op=%.3f\n", n, t * 1000.0 / n);
  fflush(stdout);
}

int main(int argc, char **argv)
{
  setvbuf(stdout, NULL, _IOLBF, 0);
  if (argc > 1 && !strcmp(argv[1], "roles"))
  {
    double seconds = argc > 2 ? atof(argv[2]) : 8;
    if (argc > 3) period_ms = atoi(argv[3]);
    return run_roles(seconds);
  }
  if (argc > 1 && !strcmp(argv[1], "bench"))
  {
    if (argc > 2) rounds = atol(argv[2]);
    printf("READY %.3f\n", qpc_ms()); fflush(stdout);
    Sleep(argc > 3 ? atoi(argv[3]) : 0);
    bench();
    Sleep(argc > 4 ? atoi(argv[4]) : 0);  // keeps the process alive until a recording window ends
    return 0;
  }
  fprintf(stderr, "usage: waittest.exe roles [seconds] [period_ms] | bench [rounds] [delay_ms] [hold_ms]\n");
  return 2;
}
