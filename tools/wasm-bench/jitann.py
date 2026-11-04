#!/usr/bin/env python3
# jitann.py <jd dir> <mode> ...: annotated JIT code.
#   top N           : the N hottest host PCs with their entry and context
#   block <hostaddr>: annotated listing of the multiblock containing hostaddr (from its entry to end)
#   func <guest off> : annotated listing for the multiblock entered at guest module offset
import sys, re, bisect, collections
d = sys.argv[1]; mode = sys.argv[2]
ranges = [tuple(int(x, 16) for x in l.split()) for l in open(d + '/ranges.txt')]
def img2host(o):
    for s, e, off in ranges:
        if off <= o < off + (e - s): return s + (o - off)
ins = {}
for l in open(d + '/image.dis'):
    m = re.match(r'\s*([0-9a-f]+):\s+(.*)', l)
    if m:
        h = img2host(int(m.group(1), 16))
        if h is not None: ins[h] = m.group(2).strip()
addrs = sorted(ins)
pcs = collections.Counter()
for l in open(d + '/pcs.txt'):
    if l.startswith('#'): total = int(l.split()[-1]); continue
    a, c = l.split(); pcs[int(a, 16)] = int(c)
ent = []
for l in open(d + '/entries.txt'):
    h, sz, nm = l.split(' ', 2); ent.append((int(h, 16), int(sz, 16), nm.strip()))
ent.sort()
estarts = [e[0] for e in ent]
def entry_of(a):
    i = bisect.bisect_right(estarts, a) - 1
    return ent[i] if i >= 0 else None
def short(nm):
    m = re.search(r'([^\\]+\.(?:exe|dll))\+0x([0-9a-f]+)', nm)
    return '%s+0x%s' % (m.group(1), m.group(2)) if m else nm[:60]
def listing(lo, hi):
    i = bisect.bisect_left(addrs, lo)
    tot = 0
    while i < len(addrs) and addrs[i] < hi:
        a = addrs[i]; c = pcs.get(a, 0); tot += c
        mark = ''
        e = entry_of(a)
        if e and e[0] == a: mark = '  <== ' + short(e[2])
        print('%6s %x: %s%s' % (c or '', a, ins[a], mark))
        i += 1
    print('# samples in range', tot)
if mode == 'top':
    n = int(sys.argv[3])
    for a, c in pcs.most_common(n):
        e = entry_of(a)
        print('%5d %5.2f%% %x %s %s' % (c, 100.0 * c / total, a, ins.get(a, '?'), short(e[2]) if e and a < e[0] + e[1] + 0x100000 else ''))
elif mode == 'range':
    listing(int(sys.argv[3], 16), int(sys.argv[4], 16))
