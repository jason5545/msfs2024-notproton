/* termwait.exe: TerminateThread on a thread that owns a mutex and waits on an event (kernel32 sync.c 395-420,
 * records 25.72). Prints how long the terminated thread's handle took to signal, what a wait on the mutex
 * returns afterwards (WAIT_ABANDONED expected), and does the same for a thread that waits on the mutex
 * while the owner is terminated. Exit 0 if both are as on Windows. */
#include <windows.h>
#include <stdio.h>
#include <string.h>
static HANDLE mtx, start_ev, stop_ev;
static DWORD WINAPI owner(void *p)
{
    WaitForSingleObject(mtx, INFINITE);
    SetEvent(start_ev);
    WaitForSingleObject(stop_ev, INFINITE);
    return 0;
}
static DWORD WINAPI waiter(void *p)
{
    DWORD r = WaitForSingleObject(mtx, 5000);
    *(DWORD *)p = r;
    if (r == WAIT_ABANDONED || r == WAIT_OBJECT_0) ReleaseMutex(mtx);
    return 0;
}
int main(int argc, char **argv)
{
    int rounds = argc > 1 ? atoi(argv[1]) : 5, bad = 0;
    /* close: the main thread closes its mutex handle before terminating the owner, as the kernel32 test
     * does; the waiter waits on the object through a handle that was closed meanwhile */
    int close_first = argc > 2 && !strcmp(argv[2], "close");
    LARGE_INTEGER f, t0, t1;
    QueryPerformanceFrequency(&f);
    for (int i = 0; i < rounds; i++) {
        DWORD wres = 0xdead;
        mtx = CreateMutexA(NULL, FALSE, NULL);
        start_ev = CreateEventA(NULL, FALSE, FALSE, NULL);
        stop_ev = CreateEventA(NULL, FALSE, FALSE, NULL);
        HANDLE o = CreateThread(NULL, 0, owner, NULL, 0, NULL);
        WaitForSingleObject(start_ev, 1000);
        HANDLE w = CreateThread(NULL, 0, waiter, &wres, 0, NULL);
        Sleep(50);
        if (close_first) { CloseHandle(mtx); mtx = NULL; }
        QueryPerformanceCounter(&t0);
        TerminateThread(o, 0);
        DWORD r = WaitForSingleObject(o, 3000);
        QueryPerformanceCounter(&t1);
        DWORD rw = WaitForSingleObject(w, 3000);
        double ms = (t1.QuadPart - t0.QuadPart) * 1000.0 / f.QuadPart;
        int ok = r == WAIT_OBJECT_0 && rw == WAIT_OBJECT_0 && wres == WAIT_ABANDONED;
        printf("round %d: owner handle %s after %.2f ms, waiter thread %s, waiter's mutex wait 0x%lx %s\n", i,
               r == WAIT_OBJECT_0 ? "signaled" : "NOT signaled", ms, rw == WAIT_OBJECT_0 ? "done" : "NOT done",
               wres, ok ? "ok" : "BAD");
        bad += !ok;
        SetEvent(stop_ev);
        CloseHandle(o); CloseHandle(w); if (mtx) CloseHandle(mtx); CloseHandle(start_ev); CloseHandle(stop_ev);
    }
    printf("RESULT %s (%d of %d bad)\n", bad ? "FAIL" : "PASS", bad, rounds);
    return bad != 0;
}
