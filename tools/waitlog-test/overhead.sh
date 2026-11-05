#!/bin/bash
# overhead.sh <label> [rounds=3] [bench rounds=50000]: waittest.exe bench under four setups, alternating, for what
# Wine patch 0061 costs: base = the runtime without the patch; off = patched, no squark-waitlog folder (no watcher);
# idle = patched, folder there (watcher thread asleep), no window; on = a window over the whole bench.
# Runtimes hadron-test/rt-wait-base and rt-wait, prefixes pfx-wait2 (no folder) and pfx-wait (folder).
# Output: ~/GitHub/msfs-src/waitlog-1105/overhead-<label>/<setup>-<n>/ and a table on stdout.
set -u
D=$(cd "$(dirname "$0")" && pwd); T=$HOME/GitHub/msfs-src/hadron-test
L=$1; N=${2:-3}; R=${3:-50000}; HOLD=${HOLD:-0}
O=$HOME/GitHub/msfs-src/waitlog-1105/overhead-$L; mkdir -p "$O"
export FEX_X87REDUCEDPRECISION=1 HB_FEX_SMC_RETIRE=1 HB_FEX_SMC_SKIP_SAME=1 HB_FEX_SMC_HOT_MAXINST=64 HB_FEX_VM_FASTPATH=1 \
  HB_FEX_SMC_DEFER_CALLRET=1 HADRON_MAC_BACKGROUND=1
for i in $(seq 1 "$N"); do
  WL=0 WL_MKDIR=0 "$D/run.sh" "$T/rt-wait-base" "$T/pfx-wait2" "$O/base-$i" "$D/build/waittest.exe" bench "$R" 1000 "$HOLD" > /dev/null 2>&1
  WL=0 WL_MKDIR=0 "$D/run.sh" "$T/rt-wait" "$T/pfx-wait2" "$O/off-$i" "$D/build/waittest.exe" bench "$R" 1000 "$HOLD" > /dev/null 2>&1
  WL=0 "$D/run.sh" "$T/rt-wait" "$T/pfx-wait" "$O/idle-$i" "$D/build/waittest.exe" bench "$R" 1000 "$HOLD" > /dev/null 2>&1
  WL=1 WL_AT=0 WL_WARMUP=0.2 WL_SECONDS=${WL_SECONDS:-8} "$D/run.sh" "$T/rt-wait" "$T/pfx-wait" "$O/on-$i" "$D/build/waittest.exe" bench "$R" 1000 "${ONHOLD:-9000}" > /dev/null 2>&1
done
python3 - "$O" "$N" <<'PY'
import sys, re, glob, statistics as st
o, n = sys.argv[1], int(sys.argv[2])
tests = ['event_pingpong', 'address_pingpong', 'critsec_uncontended', 'sleep0', 'wait_signaled_poll']
unit = {'event_pingpong': 'us/round', 'address_pingpong': 'us/round', 'critsec_uncontended': 'ns/op', 'sleep0': 'us/op', 'wait_signaled_poll': 'us/op'}
val = {}
for setup in ['base', 'off', 'idle', 'on']:
    for i in range(1, n + 1):
        try: txt = open('%s/%s-%d/stdout.txt' % (o, setup, i), errors='replace').read()
        except OSError: continue
        for t in tests:
            m = re.search(r'BENCH %s \S+ \S+=([0-9.]+)' % t, txt)
            if m: val.setdefault((setup, t), []).append(float(m.group(1)))
print('%-20s %-9s' % ('test', 'unit') + ''.join(' | %-31s' % s for s in ['base (no patch)', 'off (no folder)', 'idle (watcher)', 'on (recording)']))
for t in tests:
    row = '%-20s %-9s' % (t, unit[t])
    b = st.median(val.get(('base', t), [0])) or 1
    for s in ['base', 'off', 'idle', 'on']:
        v = val.get((s, t), [])
        if not v: row += ' | %-31s' % '-'; continue
        row += ' | %-31s' % ('%s med %.3f %+.1f%%' % ('/'.join('%.2f' % x for x in v), st.median(v), 100 * (st.median(v) / b - 1)))
    print(row)
for f in sorted(glob.glob(o + '/on-*/waitlog/*.json')):
    import json; m = json.load(open(f)); print('  %s: %d events, window %.1f s' % (f.split('/')[-3], m.get('events', -1), m.get('seconds', 0)))
PY
