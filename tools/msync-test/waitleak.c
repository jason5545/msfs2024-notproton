/* waitleak.exe [seconds]: the case Highball's msync 0014 (our 0065) fixes (records 25.72): a worker sets an
 * auto-reset event in a loop while the main thread waits on {quit, work} with WaitForMultipleObjects(WaitAny);
 * quit is never signaled. Without the fix every wait that found work signaled while it spun left a node on
 * quit's waiter list in the wineserver, which grew until memory ran out. Prints the waits done. */
#include <windows.h>
#include <stdio.h>
static HANDLE quit, work;
static volatile LONG stop;
static DWORD WINAPI setter(void *p)
{
    while (!stop) SetEvent(work);
    return 0;
}
int main(int argc, char **argv)
{
    int secs = argc > 1 ? atoi(argv[1]) : 30;
    quit = CreateEventA(NULL, TRUE, FALSE, NULL);
    work = CreateEventA(NULL, FALSE, FALSE, NULL);
    HANDLE t = CreateThread(NULL, 0, setter, NULL, 0, NULL);
    HANDLE hs[2] = { quit, work };
    ULONGLONG end = GetTickCount64() + secs * 1000ull, waits = 0, got = 0;
    while (GetTickCount64() < end) {
        DWORD r = WaitForMultipleObjects(2, hs, FALSE, 100);
        waits++; if (r == WAIT_OBJECT_0 + 1) got++;
    }
    stop = 1; WaitForSingleObject(t, 5000);
    printf("RESULT PASS waits %llu, work signaled %llu, seconds %d\n", waits, got, secs);
    return 0;
}
