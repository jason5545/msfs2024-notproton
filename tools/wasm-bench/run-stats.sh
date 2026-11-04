#!/bin/bash
# Runs a benchmark exe (bigbench.exe, fbwbench.exe: they print "PHASE warm" and "PHASE done" and hold HOLD
# seconds at each) under a Hadron runtime with FEX_PROFILESTATS=1, reading FEX's stats at both points
# into <out>/fexstats.txt. MSFS's FEX switches (compat_run.sh's recipe for 2537590) are set unless the
# environment already has them.
#
#   run-stats.sh <runtime dir> <prefix dir> <out dir> <exe>
set -u
T=$(cd "$(dirname "$0")" && pwd)
RT=$1; PFX=$2; o=$3; EXE=$4
export FEX_PROFILESTATS=1 HOLD=${HOLD:-3}
for v in FEX_X87REDUCEDPRECISION=1 HB_FEX_SMC_RETIRE=1 HB_FEX_SMC_SKIP_SAME=1 HB_FEX_SMC_HOT_MAXINST=64 \
    HB_FEX_VM_FASTPATH=1 HB_FEX_SMC_DEFER_CALLRET=1; do
  n=${v%%=*}; [ -n "${!n+x}" ] || export "$v"
done
name=$(basename "$EXE")
"$T/../hadron/run.sh" "$RT" "$PFX" "$o" "$EXE" &
bg=$!
for phase in warm done; do
  until grep -q "PHASE $phase" "$o/stdout.txt" 2>/dev/null; do sleep 0.2; kill -0 $bg 2>/dev/null || break 2; done
  pid=$(pgrep -f "$name" | while read p; do [ -f "${TMPDIR:-/tmp/}fex-$p-stats" ] && echo $p; done | head -1)
  sleep 0.5
  { echo "== $phase pid=$pid"; [ -n "$pid" ] && python3 "$T/fexstats.py" "${TMPDIR:-/tmp/}fex-$pid-stats"; } >> "$o/fexstats.txt"
done
wait $bg
echo "$(basename "$o") $(grep RESULT "$o/stdout.txt")"
grep TOTAL "$o/fexstats.txt" | sed 's/^/   /'
