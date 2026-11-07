#!/usr/bin/env python3
"""cpool_sum.py <cpool-ab.sh output>: per shape and arm, the median over rounds of the per-frame medians
(reset, record, frame) and of the whole-run process CPU a frame; b/a and c/a; timestamp checks."""
import collections, re, statistics, sys
rows = collections.defaultdict(lambda: collections.defaultdict(list)); bad = collections.Counter()
for line in open(sys.argv[1], "rb").read().decode().replace("\r", "").split("\n"):
    m = re.match(r'r(\d+) (\w) L=(\d+) A=(\d+) (.*)', line)
    if not m: continue
    arm, key, rest = m.group(2), (int(m.group(3)), int(m.group(4))), m.group(5)
    v = dict(re.findall(r'(reset|record|frame) ([\d.]+)', rest.split('p90')[0]))
    c = re.search(r'process CPU ([\d.]+) ms', rest); b = re.search(r'bad (\d+)', rest)
    if not c or not b: rows[key][arm + ':missing'].append(1); continue
    for k in ('reset', 'record', 'frame'): rows[key][arm + ':' + k].append(float(v[k]))
    rows[key][arm + ':cpu'].append(float(c.group(1)))
    bad[(key, arm)] += int(b.group(1))
cols = ('reset', 'record', 'frame', 'cpu')
print('%-14s %-3s ' % ('lists x allocs', 'arm') + ' '.join('%-24s' % k for k in cols) + ' bad')
for key in sorted(rows):
    med = {}
    for arm in 'abc':
        cells = []
        for k in cols:
            xs = rows[key].get(arm + ':' + k, [])
            med[arm, k] = statistics.median(xs) if xs else float('nan')
            cells.append('%8.3f (%s)' % (med[arm, k], '/'.join('%.2f' % x for x in xs)))
        print('%-14s %-3s ' % ('%d x %d' % key, arm) + ' '.join('%-24s' % c for c in cells) + ' %d' % bad[(key, arm)])
    print('%-14s %-3s ' % ('', 'b/a') + ' '.join('%-24s' % ('%8.3f' % (med['b', k] / med['a', k])) for k in cols))
    print('%-14s %-3s ' % ('', 'c/a') + ' '.join('%-24s' % ('%8.3f' % (med['c', k] / med['a', k])) for k in cols))
