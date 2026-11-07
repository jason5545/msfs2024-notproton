# msync-test

Tests for msync, the in-process synchronization Wine patches `wine-11.21-highball/0064`–`0068` in squark-app bring
(records 25.72). msync is on unless `WINEMSYNC=0`.

- `winetests.sh <runtime> <out> [on off]`: Wine's own tests from the prepared source tree's build folder,
  `ntdll_test` sync and om, `kernel32_test` sync and thread, aarch64-windows and i386-windows, each mode in a
  fresh prefix. Prints one line per test: exit code, Wine's summary line, the number of `Test failed` lines.
  Known on this runtime whatever the mode: i386 `thread.c:1388` (wrong error 1), now and then i386
  `thread.c:467` (a thread did not finish within 5 s) and `sync.c:1090` succeeding inside a todo block
  (counted as one failure without a `Test failed` line).
- `termwait.c` → `build/termwait.exe [rounds] [close]`: TerminateThread on a thread that owns a mutex and waits
  on an event, with another thread waiting on the mutex; the terminated thread's handle has to signal and the
  waiter has to get `WAIT_ABANDONED`. `close` closes the main thread's mutex handle first, as `kernel32_test
  sync` does (what 0068 fixes).
- `waitall.c` → `build/waitall.exe`, `build/waitall32.exe` (i686) `[rounds] [threads]`: kernel32 thread.c's
  threadFunc1 many times, each thread sets its manual-reset event and waits for all of them; counts rounds with a
  stuck thread.
- `waitleak.c` → `build/waitleak.exe [seconds]`: the shape Wine 0065 (Highball's msync 0014) fixes, a worker setting
  an auto-reset event in a loop while the main thread waits on {quit, work}; without the fix the wineserver grows.
- `rsswatch.sh <runtime> <prefix> <out> <interval> -- <command…>`: runs the command and samples that runtime's
  wineserver RSS into `<out>/rss.tsv`.

Build: `x86_64-w64-mingw32-gcc -O2 -Wall -o build/termwait.exe termwait.c` (and `waitall`, and `i686-w64-mingw32-gcc`
for `waitall32`). Run with `tools/hadron/run.sh <runtime> <throwaway prefix> <out> build/<exe> [args]`;
`WINEMSYNC=0` in the environment turns msync off for the run (the wineserver it starts inherits it).
