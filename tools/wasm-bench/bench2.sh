#!/bin/bash
# bench2.sh <label>: FBW A380 under the test runtime rt-fexexp, precise per-tick cost from the warm phase. Two runs (WARM_MS=3000 and 9000, HOLD=2) read the
# Wine process's rusage at PHASE warm; per tick = difference / difference in warm frames (about 50k ticks).
S=$(cd "$(dirname "$0")" && pwd)
L=$1
RT=~/GitHub/msfs-src/hadron-test/rt-fexexp; EXE=$S/build/fbw/fbwbench.exe
o=${BENCH_OUT:-$HOME/GitHub/msfs-src/hadron-test/bench}/$L; rm -rf "$o"; mkdir -p "$o"
sha=$(shasum -a 256 ~/GitHub/msfs-src/hadron-test/pfx-wasm/drive_c/windows/system32/xtajit64.dll | cut -c1-8)
for w in 3000 9000; do HOLD=2 WARM_MS=$w $S/rusage-run.sh $RT $o/w$w $EXE > /dev/null 2>&1; done
python3 - "$o" "$L" "$sha" <<'PY'
import sys, re
o, L, sha = sys.argv[1:]
def warm(w):
    t = open('%s/w%d/rusage.txt' % (o, w)).read().split('== done')[0]
    fr = int(re.search(r'frames=(\d+)', t).group(1))
    v = max((int(a), int(b), int(c)) for a, b, c in re.findall(r'instructions=(\d+) cycles=(\d+) user_ns=(\d+)', t))
    psi = re.search(r'green_psi=([0-9.]+)', open('%s/w%d/stdout.txt' % (o, w)).read()).group(1)
    return fr, v, psi
f1, (i1, c1, u1), p1 = warm(3000); f2, (i2, c2, u2), p2 = warm(9000)
df = f2 - f1
print('%s xtajit64=%s ticks=%d instr/tick=%.0f cycles/tick=%.0f us/tick(cpu)=%.2f IPC=%.2f psi=%s,%s' % (
    L, sha, df, (i2 - i1) / df, (c2 - c1) / df, (u2 - u1) / df * 125 / 3 / 1e3, (i2 - i1) / (c2 - c1), p1, p2))
PY
