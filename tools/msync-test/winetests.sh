#!/bin/sh
# Wine's own synchronization tests under a runtime, msync on (default) and off (WINEMSYNC=0), each mode in a
# fresh prefix (records 25.72): ntdll sync and om, kernel32 sync and thread (multi-object waits, abandoned
# mutexes, APCs and alertable waits, SignalObjectAndWait, semaphore limits, PulseEvent, keyed events,
# WaitOnAddress), the aarch64-windows and i386-windows builds from the prepared source tree's build folder.
#   tools/msync-test/winetests.sh <runtime> <out> [modes: on off]
set -u
RT=$(cd "$1" && pwd); OUT=$2; shift 2
MODES=${*:-on off}
B=${WINE_BUILD:-$HOME/GitHub/squark-app/scratch/runtimes/wine-11.21-highball/build}
here=$(cd "$(dirname "$0")" && pwd)
mkdir -p "$OUT"; OUT=$(cd "$OUT" && pwd)
for mode in $MODES; do
  pfx=$OUT/pfx-$mode; rm -rf "$pfx"
  for arch in aarch64-windows i386-windows; do
    for t in "ntdll sync" "ntdll om" "kernel32 sync" "kernel32 thread"; do
      set -- $t; dll=$1; test=$2
      exe=$B/dlls/$dll/tests/$arch/${dll}_test.exe
      o=$OUT/$mode-$arch-$dll-$test
      if [ "$mode" = off ]; then
        WINEMSYNC=0 TMO=${TMO:-600} "$here/../hadron/run.sh" "$RT" "$pfx" "$o" "$exe" "$test" > /dev/null 2>&1
      else
        TMO=${TMO:-600} "$here/../hadron/run.sh" "$RT" "$pfx" "$o" "$exe" "$test" > /dev/null 2>&1
      fi
      sum=$(grep -h -E '^[0-9a-f]+:[a-z_]+: [0-9]+ tests executed' "$o/stdout.txt" "$o/stderr.txt" 2>/dev/null | tail -1)
      fails=$(grep -h -c 'Test failed' "$o/stdout.txt" "$o/stderr.txt" 2>/dev/null | awk '{s+=$1} END {print s+0}')
      echo "$mode $arch $dll $test: $(grep -h 'exit=' "$o/run.txt") | ${sum:-no summary line} | 'Test failed' lines $fails"
    done
  done
done
