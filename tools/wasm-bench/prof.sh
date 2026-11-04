#!/bin/bash
# Profiles a benchmark exe under a Hadron runtime: FEX_BLOCKJITNAMING=1 (FEX writes /tmp/perf-<pid>.map),
# `sample` of the Wine process for SECS seconds while it warms up (WARM_MS, default 20000), then
# jitprof.py's self-time table by guest function. MSFS's FEX switches are set as in run-stats.sh.
#
#   prof.sh <runtime dir> <prefix dir> <out dir> <exe built with -g> [SECS]
set -u
T=$(cd "$(dirname "$0")" && pwd)
RT=$1; PFX=$2; o=$3; EXE=$4; SECS=${5:-8}
export FEX_BLOCKJITNAMING=1 WARM_MS=${WARM_MS:-20000}
for v in FEX_X87REDUCEDPRECISION=1 HB_FEX_SMC_RETIRE=1 HB_FEX_SMC_SKIP_SAME=1 HB_FEX_SMC_HOT_MAXINST=64 \
    HB_FEX_VM_FASTPATH=1 HB_FEX_SMC_DEFER_CALLRET=1; do
  n=${v%%=*}; [ -n "${!n+x}" ] || export "$v"
done
mkdir -p "$o"
before=$(ls /tmp/perf-*.map 2>/dev/null)
"$T/../hadron/run.sh" "$RT" "$PFX" "$o" "$EXE" &
bg=$!
map=
for _ in $(seq 100); do
  sleep 0.3
  for m in /tmp/perf-*.map; do
    [ -f "$m" ] && ! grep -qx "$m" <<< "$before" && map=$m
  done
  [ -n "$map" ] && break
done
pid=${map#/tmp/perf-}; pid=${pid%.map}
sleep 3
sample "$pid" "$SECS" -mayDie -file "$o/sample.txt" > /dev/null 2>&1
cp "$map" "$o/perf.map"
wait $bg
rm -f "$map"
grep RESULT "$o/stdout.txt"
python3 "$T/jitprof.py" "$o/sample.txt" "$o/perf.map" "$EXE" ${TOP:-30} | tee "$o/jitprof.txt"
