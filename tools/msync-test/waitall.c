/* waitall.exe [rounds] [threads]: kernel32 thread.c's threadFunc1 shape many times (records 25.72): each of
 * <threads> threads sets its own manual-reset event, then waits for all of them with
 * WaitForMultipleObjects(WaitAll); the main thread waits 5 s for every thread. Counts rounds where a thread
 * did not finish (a lost wake-up of a wait-all). Exit 0 if none. */
#include <windows.h>
#include <stdio.h>
#define MAXT 64
static HANDLE ev[MAXT];
static int nt;
static DWORD WINAPI fn(void *p)
{
    SetEvent(ev[(INT_PTR)p]);
    return WaitForMultipleObjects(nt, ev, TRUE, 10000);
}
int main(int argc, char **argv)
{
    int rounds = argc > 1 ? atoi(argv[1]) : 2000, bad = 0, timeouts = 0;
    nt = argc > 2 ? atoi(argv[2]) : 4;
    HANDLE th[MAXT];
    for (int r = 0; r < rounds; r++) {
        for (int i = 0; i < nt; i++) ev[i] = CreateEventA(NULL, TRUE, FALSE, NULL);
        for (int i = 0; i < nt; i++) th[i] = CreateThread(NULL, 0, fn, (void *)(INT_PTR)i, 0, NULL);
        int b = 0;
        for (int i = 0; i < nt; i++) {
            DWORD w = WaitForSingleObject(th[i], 5000), code = 0;
            if (w != WAIT_OBJECT_0) { b = 1; TerminateThread(th[i], 99); WaitForSingleObject(th[i], 1000); }
            GetExitCodeThread(th[i], &code);
            if (code == WAIT_TIMEOUT) timeouts++;
            CloseHandle(th[i]);
        }
        for (int i = 0; i < nt; i++) CloseHandle(ev[i]);
        if (b && bad < 5) printf("round %d: a thread did not finish within 5 s\n", r);
        bad += b;
    }
    printf("RESULT %s (%d of %d rounds with a stuck thread, %d waits timed out) threads=%d\n", bad ? "FAIL" : "PASS",
           bad, rounds, timeouts, nt);
    return bad != 0;
}
