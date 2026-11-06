# tptimer-test

Thread pool timers (`CreateThreadpoolTimer`／`SetThreadpoolTimer`) in the shapes that decide how Wine's timer
queue thread (`wine_threadpool_timerqueue`, `dlls/ntdll/threadpool.c`) waits. Written for Squark Wine patch 0062
(the thread spun whenever no relative timer was pending); see `records/logs/notproton-log.md` 25.61.

- `tptimer.c`: `tptimer.exe <mode> [seconds=5] [ms] [timers=1]`, modes `none idle abs rel per absper` (see the
  comment at the top). Prints `READY`, then callbacks, intervals, the first callback's lateness and the process CPU.
- `build.sh`: x64 build with llvm-mingw from the Squark app's scratch dir.
- `ab.sh <out> <prefix> <runtime>...`: every mode under every runtime, alternating, with Wine patch 0061's wait
  recorder on the timer queue thread (`tools/waitlog-test/run.sh`, `tools/waitlog/waitlog.py`) and the
  wineserver's CPU over the same window; one line per run in `<out>/summary.txt`.
