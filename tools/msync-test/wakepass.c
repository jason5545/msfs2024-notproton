/* wakepass.exe <sem|event|mutex|all> [rounds] [chaos_ms] [checks]: lost wake-ups when msync wakes one
 * single-object waiter, not all of them, for an operation that frees one unit (Wine 0069, records 25.73).
 *
 * One object (a semaphore, an auto-reset event or a mutex) and these threads on it:
 *   6 wait INFINITE (msync: asleep on the object's ulock);
 *   4 wait 0-3 ms and time out often;                                              (chaos phase only)
 *   3 wait alertable (INFINITE or 1-20 ms) and get user APCs;                      (chaos phase only)
 *   2 wait for all of {object, gate}, gate a manual-reset event that is reset nearly always: a wait-all
 *     thread waits for the object without taking it, so one woken for it goes to sleep on the gate;
 *   1 waits for all of {object, side mutex} while another thread takes the side mutex in a loop: the
 *     wait-all takes the object, misses the side mutex and puts the object back (msync's tooslow);
 *   1 disturber that queues APCs to the alertable ones and suspends and resumes the others (in Wine a
 *     suspend is a signal, so an ulock wait returns EINTR).                        (chaos phase only)
 * A round is a chaos phase (the main thread releases / sets / holds the object at random, the gate opens
 * now and then) and a check phase with only the INFINITE and wait-all threads: the main thread releases
 * 1-3 (sets the event, holds and releases the mutex), sometimes after a 1 ms sleep so that every waiter
 * is asleep, and the object has to be taken within 2 s. A lost wake-up leaves it free with the INFINITE
 * waiters asleep ("stuck"). At the end: every thread finishes, units released = taken + left over
 * (semaphore, event), every APC ran. Before that, for each object, "absorb" (run_absorb below): the
 * smallest shape of the wait-all case, 40 releases.  Exit 0 if all of that holds for each object. */
#include <windows.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

enum { K_SEM, K_EVENT, K_MUTEX };
static const char *kind_name[] = { "sem", "event", "mutex" };

#define NA 6
#define NB 4
#define NC 3
#define ND 2

typedef struct { LONG CurrentCount; LONG MaximumCount; } SEMINFO;
typedef struct { int EventType; LONG EventState; } EVINFO;
typedef struct { LONG CurrentCount; BOOLEAN OwnedByCaller; BOOLEAN AbandonedState; } MUTINFO;
static LONG (WINAPI *pNtQuerySemaphore)(HANDLE, int, void *, ULONG, ULONG *);
static LONG (WINAPI *pNtQueryEvent)(HANDLE, int, void *, ULONG, ULONG *);
static LONG (WINAPI *pNtQueryMutant)(HANDLE, int, void *, ULONG, ULONG *);
static LONG (WINAPI *pNtSetEvent)(HANDLE, LONG *);

static int kind;
static HANDLE obj, gate, side_mtx;
static volatile LONG phase;   /* 0 chaos, 1 check, 2 stop */
static volatile LONG parked;
static volatile LONG64 released, acquired, timeouts, apcs_queued, apcs_run, io_compl, suspends, wa_ok, wa_to,
    tooslow_tries, bad_ret;
static HANDLE th_a[NA], th_b[NB], th_c[NC];

static unsigned rnd(unsigned *s) { *s ^= *s << 13; *s ^= *s >> 17; *s ^= *s << 5; return *s; }
static LONGLONG qpc(void) { LARGE_INTEGER t; QueryPerformanceCounter(&t); return t.QuadPart; }
static LONGLONG qpf;
static void spin_us(unsigned us) { LONGLONG end = qpc() + qpf * us / 1000000; while (qpc() < end) YieldProcessor(); }

static LONG obj_units(void)   /* semaphore count, event state, 1 if the mutex is free */
{
    ULONG len;
    if (kind == K_SEM) { SEMINFO i; pNtQuerySemaphore(obj, 0, &i, sizeof(i), &len); return i.CurrentCount; }
    if (kind == K_EVENT) { EVINFO i; pNtQueryEvent(obj, 0, &i, sizeof(i), &len); return i.EventState; }
    { MUTINFO i; pNtQueryMutant(obj, 0, &i, sizeof(i), &len); return i.CurrentCount; }
}

static void took(void)
{
    InterlockedIncrement64(&acquired);
    if (kind == K_MUTEX) { spin_us(5); ReleaseMutex(obj); }
}

static void check_ret(DWORD r)
{
    if (r != WAIT_OBJECT_0 && r != WAIT_TIMEOUT && r != WAIT_IO_COMPLETION) {
        if (InterlockedIncrement64(&bad_ret) <= 5) printf("  unexpected wait result %#lx\n", r);
    }
}

static DWORD WINAPI a_main(void *p)   /* INFINITE */
{
    for (;;) {
        DWORD r = WaitForSingleObject(obj, INFINITE);
        check_ret(r);
        if (r == WAIT_OBJECT_0) took();
        if (phase == 2) return 0;
        if (kind == K_MUTEX) Sleep(0);
    }
}

static void park(int alertable)
{
    InterlockedIncrement(&parked);
    while (phase == 1) { if (alertable) SleepEx(1, TRUE); else Sleep(1); }
    InterlockedDecrement(&parked);
}

static DWORD WINAPI b_main(void *p)   /* short timeouts */
{
    unsigned s = 0x9e3779b9u * (unsigned)(INT_PTR)p + 1;
    while (phase != 2) {
        if (phase == 1) { park(0); continue; }
        DWORD r = WaitForSingleObject(obj, rnd(&s) % 4);
        check_ret(r);
        if (r == WAIT_OBJECT_0) took();
        else if (r == WAIT_TIMEOUT) InterlockedIncrement64(&timeouts);
    }
    return 0;
}

static void CALLBACK apc_fn(ULONG_PTR p) { InterlockedIncrement64(&apcs_run); }

static DWORD WINAPI c_main(void *p)   /* alertable */
{
    unsigned s = 0x85ebca6bu * (unsigned)(INT_PTR)p + 7;
    while (phase != 2) {
        if (phase == 1) { park(1); continue; }
        DWORD tmo = (rnd(&s) % 3) ? 1 + rnd(&s) % 20 : INFINITE;
        DWORD r = WaitForSingleObjectEx(obj, tmo, TRUE);
        check_ret(r);
        if (r == WAIT_OBJECT_0) took();
        else if (r == WAIT_TIMEOUT) InterlockedIncrement64(&timeouts);
        else if (r == WAIT_IO_COMPLETION) InterlockedIncrement64(&io_compl);
    }
    while (SleepEx(0, TRUE) == WAIT_IO_COMPLETION);
    return 0;
}

static DWORD WINAPI d_main(void *p)   /* wait-all {obj, gate} */
{
    HANDLE hs[2] = { obj, gate };
    while (phase != 2) {
        DWORD r = WaitForMultipleObjects(2, hs, TRUE, phase == 1 ? INFINITE : 50);
        check_ret(r);
        if (r == WAIT_OBJECT_0) { InterlockedIncrement64(&wa_ok); took(); }
        else if (r == WAIT_TIMEOUT) InterlockedIncrement64(&wa_to);
    }
    return 0;
}

static DWORD WINAPI e_main(void *p)   /* wait-all {obj, side mutex} */
{
    HANDLE hs[2] = { obj, side_mtx };
    while (phase != 2) {
        DWORD r = WaitForMultipleObjects(2, hs, TRUE, phase == 1 ? INFINITE : 20);
        check_ret(r);
        if (r == WAIT_OBJECT_0) { InterlockedIncrement64(&wa_ok); ReleaseMutex(side_mtx); took(); }
        else if (r == WAIT_TIMEOUT) InterlockedIncrement64(&wa_to);
    }
    return 0;
}

static DWORD WINAPI holder_main(void *p)   /* takes the side mutex in a loop */
{
    while (phase != 2) {
        if (WaitForSingleObject(side_mtx, 100) == WAIT_OBJECT_0) {
            InterlockedIncrement64(&tooslow_tries);
            spin_us(20);
            ReleaseMutex(side_mtx);
        }
        spin_us(10);
    }
    return 0;
}

static DWORD WINAPI disturber_main(void *p)
{
    unsigned s = 12345;
    while (phase != 2) {
        if (phase == 1) { park(0); continue; }
        unsigned x = rnd(&s);
        if (x % 2) {
            if (QueueUserAPC(apc_fn, th_c[x / 2 % NC], 0)) InterlockedIncrement64(&apcs_queued);
        } else {
            HANDLE t = (x / 2 % 2) ? th_a[x / 4 % NA] : th_b[x / 4 % NB];
            if (SuspendThread(t) != (DWORD)-1) { spin_us(rnd(&s) % 50); ResumeThread(t); InterlockedIncrement64(&suspends); }
        }
        spin_us(rnd(&s) % 200);
    }
    return 0;
}

static void main_release(unsigned n)   /* semaphore n, event set; counts units that came into being */
{
    if (kind == K_SEM) {
        LONG prev;
        if (ReleaseSemaphore(obj, n, &prev)) InterlockedAdd64(&released, n);
    } else if (kind == K_EVENT) {
        LONG prev = 1;
        pNtSetEvent(obj, &prev);
        if (!prev) InterlockedIncrement64(&released);
    }
}

static int wait_taken(LONGLONG a0)   /* the check: taken within 2 s */
{
    LONGLONG end = qpc() + 2 * qpf;
    while (qpc() < end) {
        if (kind == K_MUTEX ? acquired > a0 : obj_units() == 0) return 1;
        spin_us(20);
    }
    return 0;
}

static int run_kind(int k, int rounds, int chaos_ms, int checks)
{
    HANDLE th[64], endw[64];   /* endw: the threads that wait INFINITE in the check phase */
    int nendw = 0, nt = 0, i, r, stuck = 0, drain_stuck = 0, hung = 0, park_to = 0;
    unsigned s = 777;

    kind = k; phase = 0; parked = 0;
    released = acquired = timeouts = apcs_queued = apcs_run = io_compl = suspends = wa_ok = wa_to = tooslow_tries = bad_ret = 0;
    obj = k == K_SEM ? CreateSemaphoreA(NULL, 0, 0x7fffffff, NULL)
        : k == K_EVENT ? CreateEventA(NULL, FALSE, FALSE, NULL) : CreateMutexA(NULL, FALSE, NULL);
    gate = CreateEventA(NULL, TRUE, FALSE, NULL);
    side_mtx = CreateMutexA(NULL, FALSE, NULL);

    for (i = 0; i < NA; i++) th[nt++] = th_a[i] = CreateThread(NULL, 0, a_main, (void *)(INT_PTR)i, 0, NULL);
    for (i = 0; i < NB; i++) th[nt++] = th_b[i] = CreateThread(NULL, 0, b_main, (void *)(INT_PTR)i, 0, NULL);
    for (i = 0; i < NC; i++) th[nt++] = th_c[i] = CreateThread(NULL, 0, c_main, (void *)(INT_PTR)i, 0, NULL);
    for (i = 0; i < NA; i++) endw[nendw++] = th_a[i];
    for (i = 0; i < ND; i++) endw[nendw++] = th[nt++] = CreateThread(NULL, 0, d_main, NULL, 0, NULL);
    endw[nendw++] = th[nt++] = CreateThread(NULL, 0, e_main, NULL, 0, NULL);
    th[nt++] = CreateThread(NULL, 0, holder_main, NULL, 0, NULL);
    th[nt++] = CreateThread(NULL, 0, disturber_main, NULL, 0, NULL);

    for (r = 0; r < rounds; r++) {
        LONGLONG end;
        int rstuck = 0;

        phase = 0;
        end = qpc() + qpf * chaos_ms / 1000;
        while (qpc() < end) {
            unsigned x = rnd(&s);
            if (kind == K_MUTEX) {
                if (WaitForSingleObject(obj, 5) == WAIT_OBJECT_0) { spin_us(x % 30); ReleaseMutex(obj); }
            } else if (obj_units() < 4) main_release(kind == K_SEM ? 1 + x % 4 : 1);
            if (x % 64 == 0) { SetEvent(gate); spin_us(x % 100); ResetEvent(gate); }
            if (x % 8 == 0) Sleep(0); else spin_us(x % 50);
        }

        phase = 1;
        ResetEvent(gate);
        for (i = 0; i < NC; i++) if (QueueUserAPC(apc_fn, th_c[i], 0)) InterlockedIncrement64(&apcs_queued);
        end = qpc() + 5 * qpf;
        while (parked < NB + NC + 1 && qpc() < end) Sleep(1);
        if (parked < NB + NC + 1) { park_to++; printf("  %s round %d: only %ld of %d threads parked\n", kind_name[k], r, parked, NB + NC + 1); }

        if (kind != K_MUTEX && !wait_taken(0)) {
            drain_stuck++;
            printf("  %s round %d: %ld left at the start of the check phase for 2 s\n", kind_name[k], r, obj_units());
        }
        /* the wait-all threads asleep on the object itself, not on the gate: open the gate while the
         * object is taken, let them get past it, close it again */
        if (kind == K_MUTEX) WaitForSingleObject(obj, 2000);
        SetEvent(gate); Sleep(5); ResetEvent(gate); Sleep(2);
        if (kind == K_MUTEX) ReleaseMutex(obj);
        for (i = 0; i < checks && !rstuck; i++) {
            unsigned x = rnd(&s);
            LONGLONG a0 = acquired;
            if (x % 4 == 0) Sleep(1); else spin_us(x % 200);
            if (kind == K_MUTEX) {
                if (WaitForSingleObject(obj, 2000) != WAIT_OBJECT_0) { rstuck = 1; printf("  mutex round %d check %d: main could not take it\n", r, i); break; }
                spin_us(50);
                a0 = acquired;
                ReleaseMutex(obj);
            } else main_release(kind == K_SEM ? 1 + x % 3 : 1);
            if (!wait_taken(a0)) {
                rstuck = 1;
                printf("  %s round %d check %d: not taken within 2 s (units %ld)\n", kind_name[k], r, i, obj_units());
            }
        }
        stuck += rstuck;
    }

    phase = 2;
    SetEvent(gate);
    {
        LONGLONG end = qpc() + 5 * qpf;
        while (WaitForMultipleObjects(nendw, endw, TRUE, 0) == WAIT_TIMEOUT && qpc() < end) { main_release(1); Sleep(1); }
    }
    for (i = 0; i < nt; i++) {
        if (WaitForSingleObject(th[i], 5000) != WAIT_OBJECT_0) { hung++; TerminateThread(th[i], 99); }
        CloseHandle(th[i]);
    }

    {
        LONG left = kind == K_MUTEX ? 0 : obj_units();
        int acct = kind == K_MUTEX || released == acquired + left;
        int apc_ok = apcs_run == apcs_queued;
        int ok = !stuck && !drain_stuck && !hung && !park_to && acct && apc_ok && !bad_ret;
        printf("RESULT %s kind=%s rounds=%d stuck=%d drain_stuck=%d hung=%d park_timeout=%d released=%lld taken=%lld left=%ld "
               "(%s) timeouts=%lld apcs=%lld/%lld io_completion=%lld suspends=%lld waitall_ok=%lld waitall_timeout=%lld "
               "side_holds=%lld bad_ret=%lld\n",
               ok ? "PASS" : "FAIL", kind_name[k], rounds, stuck, drain_stuck, hung, park_to, released, acquired, left,
               kind == K_MUTEX ? "no count" : acct ? "adds up" : "DOES NOT ADD UP", timeouts, apcs_run, apcs_queued,
               io_compl, suspends, wa_ok, wa_to, tooslow_tries, bad_ret);
        fflush(stdout);
        CloseHandle(obj); CloseHandle(gate); CloseHandle(side_mtx);
        return ok;
    }
}

/* absorb: one thread waits INFINITE on the object, one waits for all of {object, gate} with the gate closed,
 * both asleep on the object (the main thread holds the mutex meanwhile); a release has to reach the first
 * within 1 s. Without handing the wake on, the wait-all thread takes it about half the time and goes to
 * sleep on the gate. A miss is counted and undone by opening the gate. */
static HANDLE go_ev;
static volatile LONG ab_stop, ab_a, ab_d;
static DWORD WINAPI ab_a_main(void *p)
{
    for (;;) {
        WaitForSingleObject(go_ev, INFINITE);
        if (ab_stop) return 0;
        if (WaitForSingleObject(obj, INFINITE) == WAIT_OBJECT_0) {
            InterlockedIncrement(&ab_a);
            if (kind == K_MUTEX) ReleaseMutex(obj);
        }
        if (ab_stop) return 0;
    }
}
static DWORD WINAPI ab_d_main(void *p)
{
    HANDLE hs[2] = { obj, gate };
    for (;;) {
        DWORD r = WaitForMultipleObjects(2, hs, TRUE, INFINITE);
        if (ab_stop) return 0;
        if (r == WAIT_OBJECT_0) { InterlockedIncrement(&ab_d); if (kind == K_MUTEX) ReleaseMutex(obj); }
    }
}
static int run_absorb(int k, int n)
{
    HANDLE ta, td;
    int i, missed = 0, hung = 0;

    kind = k; ab_stop = 0; ab_a = ab_d = 0;
    obj = k == K_SEM ? CreateSemaphoreA(NULL, 0, 0x7fffffff, NULL)
        : k == K_EVENT ? CreateEventA(NULL, FALSE, FALSE, NULL) : CreateMutexA(NULL, FALSE, NULL);
    gate = CreateEventA(NULL, TRUE, FALSE, NULL);
    go_ev = CreateEventA(NULL, FALSE, FALSE, NULL);
    ta = CreateThread(NULL, 0, ab_a_main, NULL, 0, NULL);
    td = CreateThread(NULL, 0, ab_d_main, NULL, 0, NULL);
    for (i = 0; i < n; i++) {
        LONG a0 = ab_a;
        DWORD t;
        if (kind == K_MUTEX) WaitForSingleObject(obj, INFINITE);
        SetEvent(go_ev);
        SetEvent(gate); Sleep(10); ResetEvent(gate); Sleep(10);
        if (kind == K_MUTEX) ReleaseMutex(obj); else main_release(1);
        t = GetTickCount();
        while (ab_a == a0 && GetTickCount() - t < 1000) Sleep(1);
        if (ab_a == a0) { missed++; SetEvent(gate); Sleep(20); ResetEvent(gate); while (ab_a == a0 && GetTickCount() - t < 3000) Sleep(1); }
    }
    ab_stop = 1;
    SetEvent(gate); SetEvent(go_ev);
    if (kind == K_SEM) ReleaseSemaphore(obj, 4, NULL); else if (kind == K_EVENT) SetEvent(obj);
    if (WaitForSingleObject(ta, 2000)) { hung++; TerminateThread(ta, 99); }
    if (kind == K_EVENT) SetEvent(obj);
    if (WaitForSingleObject(td, 2000)) { hung++; TerminateThread(td, 99); }
    printf("RESULT %s absorb kind=%s releases=%d missed=%d (taken by the wait-all instead: %ld) hung=%d\n",
           !missed && !hung ? "PASS" : "FAIL", kind_name[k], n, missed, ab_d, hung);
    CloseHandle(ta); CloseHandle(td); CloseHandle(obj); CloseHandle(gate); CloseHandle(go_ev);
    return !missed && !hung;
}

int main(int argc, char **argv)
{
    const char *which = argc > 1 ? argv[1] : "all";
    int rounds = argc > 2 ? atoi(argv[2]) : 20, chaos_ms = argc > 3 ? atoi(argv[3]) : 300,
        checks = argc > 4 ? atoi(argv[4]) : 200, ok = 1, k;
    HMODULE nt = GetModuleHandleA("ntdll.dll");
    LARGE_INTEGER f;

    QueryPerformanceFrequency(&f); qpf = f.QuadPart;
    pNtQuerySemaphore = (void *)GetProcAddress(nt, "NtQuerySemaphore");
    pNtQueryEvent = (void *)GetProcAddress(nt, "NtQueryEvent");
    pNtQueryMutant = (void *)GetProcAddress(nt, "NtQueryMutant");
    pNtSetEvent = (void *)GetProcAddress(nt, "NtSetEvent");
    setvbuf(stdout, NULL, _IONBF, 0);
    printf("wakepass %s rounds=%d chaos_ms=%d checks=%d\n", which, rounds, chaos_ms, checks);
    for (k = K_SEM; k <= K_MUTEX; k++)
        if (!strcmp(which, "all") || !strcmp(which, kind_name[k])) ok &= run_absorb(k, 40);
    for (k = K_SEM; k <= K_MUTEX; k++)
        if (!strcmp(which, "all") || !strcmp(which, kind_name[k])) ok &= run_kind(k, rounds, chaos_ms, checks);
    printf("RESULT %s\n", ok ? "PASS" : "FAIL");
    return !ok;
}
