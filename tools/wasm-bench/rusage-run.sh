#!/bin/bash
# rusage-run.sh <runtime> <out> <exe>: runs a benchmark exe (PHASE warm / PHASE done, HOLD) in the test prefix pfx-wasm
# under a Hadron runtime and reads proc_pid_rusage of the Wine process (instructions, cycles) at both points.
# MSFS's FEX switches are set as in run-stats.sh. build/pidrusage is built from pidrusage.c on first use.
set -u
S=$(cd "$(dirname "$0")" && pwd)
RT=$1; o=$2; EXE=$3; name=$(basename "$EXE")
export HOLD=${HOLD:-2}
for v in FEX_X87REDUCEDPRECISION=1 HB_FEX_SMC_RETIRE=1 HB_FEX_SMC_SKIP_SAME=1 HB_FEX_SMC_HOT_MAXINST=64 \
    HB_FEX_VM_FASTPATH=1 HB_FEX_SMC_DEFER_CALLRET=1; do
  n=${v%%=*}; [ -n "${!n+x}" ] || export "$v"
done
[ -x "$S/build/pidrusage" ] || clang -O2 -o "$S/build/pidrusage" "$S/pidrusage.c"
rm -rf "$o"; mkdir -p "$o"
"$S/../hadron/run.sh" "$RT" "${PFX:-$HOME/GitHub/msfs-src/hadron-test/pfx-wasm}" "$o" "$EXE" &
bg=$!
for phase in warm done; do
  until grep -q "PHASE $phase" "$o/stdout.txt" 2>/dev/null; do sleep 0.1; kill -0 $bg 2>/dev/null || break 2; done
  sleep 0.3
  pids=$(pgrep -f "$name" | tr '\n' ' ')
  echo "== $phase $(grep "PHASE $phase" "$o/stdout.txt")" >> "$o/rusage.txt"
  "$S/build/pidrusage" $pids >> "$o/rusage.txt"
done
wait $bg
grep RESULT "$o/stdout.txt"
cat "$o/rusage.txt"
