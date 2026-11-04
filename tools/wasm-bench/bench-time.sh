#!/bin/bash
# bench-time.sh <label> [timing runs, default 2]: FBW A380 under rt-fexexp: timing runs (HOLD=0 WARM_MS=10000) and
# one counting run (HOLD=2 WARM_MS=3000, rusage of the Wine process between PHASE warm and PHASE done = 600 ticks).
S=$(cd "$(dirname "$0")" && pwd)
L=$1; N=${2:-2}
RT=~/GitHub/msfs-src/hadron-test/rt-fexexp; EXE=$S/build/fbw/fbwbench.exe
o=${BENCH_OUT:-$HOME/GitHub/msfs-src/hadron-test/bench}/time-$L; rm -rf "$o"; mkdir -p "$o"
sha=$(shasum -a 256 ~/GitHub/msfs-src/hadron-test/pfx-wasm/drive_c/windows/system32/xtajit64.dll | cut -c1-8)
for i in $(seq $N); do
  HOLD=0 WARM_MS=10000 $S/rusage-run.sh $RT $o/t$i $EXE > /dev/null 2>&1
  grep RESULT $o/t$i/stdout.txt | sed "s/^/$L t$i /"
done
HOLD=2 WARM_MS=3000 $S/rusage-run.sh $RT $o/c $EXE > /dev/null 2>&1
python3 - "$o/c/rusage.txt" "$L" "$sha" <<'PY'
import sys, re
t = open(sys.argv[1]).read().split('== done')
def big(s):
    v = [(int(a), int(b)) for a, b in re.findall(r'instructions=(\d+) cycles=(\d+)', s)]
    return max(v)
iw, cw = big(t[0]); id_, cd = big(t[1])
print('%s xtajit64=%s instr/tick=%.0f cycles/tick(after hold)=%.0f' % (sys.argv[2], sys.argv[3], (id_ - iw) / 600, (cd - cw) / 600))
PY
grep RESULT $o/c/stdout.txt | grep -o 'green_psi=[0-9.]*' | sed "s/^/$L /"
