#!/usr/bin/env python3
# jitimg.py <jd dir>: builds image.bin (union of the dumped blocks, one file per contiguous range) + ranges.txt,
# and pcs.txt (self samples per host PC from sample.txt leaf frames).
import sys, re, collections
d = sys.argv[1]
rows = []
for l in open(d + '/code.idx'):
    h, sz, off, nm = l.split(' ', 3)
    rows.append((int(h, 16), int(sz, 16), int(off, 16), nm.strip()))
rows.sort()
binf = open(d + '/code.bin', 'rb')
ranges = []  # (start, end, bytes)
cur = None
for h, sz, off, nm in rows:
    if cur and h <= cur[1]:
        if h + sz > cur[1]:
            binf.seek(off + (cur[1] - h)); cur[2].extend(binf.read(h + sz - cur[1])); cur[1] = h + sz
    else:
        if cur: ranges.append(cur)
        binf.seek(off); cur = [h, h + sz, bytearray(binf.read(sz))]
ranges.append(cur)
with open(d + '/image.bin', 'wb') as f, open(d + '/ranges.txt', 'w') as r:
    o = 0
    for s, e, b in ranges:
        f.write(b); r.write('%x %x %x\n' % (s, e, o)); o += len(b)
# entries: host start -> name (first registration of each start)
with open(d + '/entries.txt', 'w') as f:
    seen = set()
    for h, sz, off, nm in rows:
        if h in seen: continue
        seen.add(h); f.write('%x %x %s\n' % (h, sz, nm))
# self samples per PC
node = re.compile(r'^([ +!:|]*)(\d+) (.*?)(?:\s+\[(0x[0-9a-f]+)(?:,0x[0-9a-f]+)*\])?\s*$')
rowsS = []; in_tree = False
for line in open(d + '/sample.txt', errors='replace'):
    if line.startswith('Call graph:'): in_tree = True; continue
    if in_tree and (line.startswith('Total number in stack') or line.startswith('Sort by top of stack')): break
    if not in_tree: continue
    m = node.match(line.rstrip('\n'))
    if m: rowsS.append((len(m.group(1)), int(m.group(2)), m.group(3), m.group(4)))
pcs = collections.Counter(); total = 0
for i, (depth, cnt, sym, addr) in enumerate(rowsS):
    child = 0; nxt = None
    for d2, c2, _, _ in rowsS[i + 1:]:
        if d2 <= depth: break
        if nxt is None: nxt = d2
        if d2 == nxt: child += c2
    s = cnt - child
    if s > 0:
        total += s
        if addr: pcs[int(addr, 16)] += s
with open(d + '/pcs.txt', 'w') as f:
    f.write('# total self samples %d\n' % total)
    for a, c in pcs.most_common(): f.write('%x %d\n' % (a, c))
print('ranges', len(ranges), 'bytes', sum(e - s for s, e, _ in ranges), 'samples', total, 'pcs', len(pcs))
