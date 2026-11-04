#!/bin/bash
# Runs build/big-<NF>/bigbench.exe under a Hadron runtime with FEX_PROFILESTATS=1 and reads FEX's stats
# after the warm-up frames and after the measured ones (bigbench holds HOLD seconds at each point).
# MSFS's FEX switches (compat_run.sh's recipe for 2537590) are set unless already in the environment.
#
#   run-big.sh <runtime dir> <prefix dir> <out dir> <NF>...
set -u
T=$(cd "$(dirname "$0")" && pwd)
RT=$1; PFX=$2; OUT=$3; shift 3
export FEX_PROFILESTATS=1 HOLD=${HOLD:-3}
for v in FEX_X87REDUCEDPRECISION=1 HB_FEX_SMC_RETIRE=1 HB_FEX_SMC_SKIP_SAME=1 HB_FEX_SMC_HOT_MAXINST=64 \
    HB_FEX_VM_FASTPATH=1 HB_FEX_SMC_DEFER_CALLRET=1; do
  n=${v%%=*}; [ -n "${!n+x}" ] || export "$v"
done
for nf in "$@"; do
  o=$OUT/big-$nf
  "$T/../hadron/run.sh" "$RT" "$PFX" "$o" "$T/build/big-$nf/bigbench.exe" &
  bg=$!
  for phase in warm done; do
    until grep -q "PHASE $phase" "$o/stdout.txt" 2>/dev/null; do sleep 0.2; kill -0 $bg 2>/dev/null || break 2; done
    pid=$(pgrep -f 'bigbench.exe' | while read p; do [ -f "${TMPDIR:-/tmp/}fex-$p-stats" ] && echo $p; done | head -1)
    sleep 0.5
    { echo "== $phase pid=$pid"; [ -n "$pid" ] && python3 "$T/fexstats.py" "${TMPDIR:-/tmp/}fex-$pid-stats"; } >> "$o/fexstats.txt"
  done
  wait $bg
  echo "big-$nf $(grep RESULT "$o/stdout.txt") | $(grep TOTAL "$o/fexstats.txt" | tr '\n' '|')"
done
