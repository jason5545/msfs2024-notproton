#!/bin/bash
# run.sh <runtime> <prefix> <out> <exe> [args...]: runs a Windows program with tools/hadron/run.sh and records its
# waits (Wine patch 0061) once it prints READY: WL_AT seconds later (default 1) a window of WL_SECONDS (default 4,
# warmup WL_WARMUP, default 0.5) over the process WL_EXE (default the exe's name). WL=0 runs it without a window.
# The prefix gets squark-waitlog/ before the program starts (the recorder only watches when the folder exists).
# Output: <out>/stdout.txt etc. from run.sh, the recording in <out>/waitlog/.
set -u
T=$(cd "$(dirname "$0")/../.." && pwd)
RT=$1; PFX=$2; OUT=$3; EXE=$4; shift 4
WL=${WL:-1}; AT=${WL_AT:-1}; SECS=${WL_SECONDS:-4}; WARM=${WL_WARMUP:-0.5}; WEXE=${WL_EXE:-$(basename "$EXE")}
# WL_MKDIR=0 leaves the folder out (the recorder then does not even start its watcher thread)
[ "${WL_MKDIR:-1}" = 1 ] && mkdir -p "$PFX/squark-waitlog"
mkdir -p "$OUT"; OUT=$(cd "$OUT" && pwd); rm -rf "$OUT/waitlog"; mkdir -p "$OUT/waitlog"
rm -f "$OUT/stdout.txt"
"$T/tools/hadron/run.sh" "$RT" "$PFX" "$OUT" "$EXE" "$@" &
bg=$!
if [ "$WL" = 1 ]; then
  until grep -q '^READY' "$OUT/stdout.txt" 2>/dev/null; do kill -0 $bg 2>/dev/null || break; sleep 0.05; done
  sleep "$AT"
  id=t$(date +%H%M%S)$$
  printf 'id=%s\nexe=%s\nseconds=%s\nwarmup=%s\nout=%s\n%s' "$id" "$WEXE" "$SECS" "$WARM" "$OUT/waitlog" "${WL_EXTRA:-}" \
    > "$PFX/squark-waitlog/ctl.tmp"
  mv "$PFX/squark-waitlog/ctl.tmp" "$PFX/squark-waitlog/ctl"
  echo "ctl id=$id at $(perl -MTime::HiRes=time -MPOSIX=strftime -e '$t=time; printf "%s.%03d", strftime("%H:%M:%S", localtime $t), ($t-int $t)*1000')" > "$OUT/waitlog/ctl.txt"
fi
wait $bg
if [ "$WL" = 1 ]; then
  for _ in $(seq 1 100); do ls "$OUT"/waitlog/*.json > /dev/null 2>&1 && break; sleep 0.1; done
  ls -la "$OUT/waitlog"
fi
cat "$OUT/run.txt" | tail -2
