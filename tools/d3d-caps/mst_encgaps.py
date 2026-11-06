#!/usr/bin/env python3
"""GPU idle gaps of one process and what sits in them (records 25.59).

  mst_encgaps.py <metal-gpu-intervals.xml> <pid> [frames]

Reads the GPU instrument's intervals (xctrace export), keeps the depth-0 intervals of <pid>, merges overlaps and
looks at each gap between them:
  - per size bucket: count, idle share, and how far the encoder id steps across the gap. Neighbouring encoders
    of one command buffer have consecutive ids; a step of 2 or more means encoders that left no interval
    sit in the gap (a timestamp counter heap resolve does that, records 25.59);
  - the 40-160 us gaps: median size, and their neighbours: a small Compute (< 6 us, like KosmicKrisp's
    write_u32 before a timestamp) before or after, and whether the interval after is the last one of its
    command buffer id (like the query copy at the end of a list). Command buffer ids are reused over a trace
    (MTL4 command buffers), so they only tell two neighbouring intervals apart.
<frames> divides counts and idle time per frame."""
import statistics as st
import sys
import xml.etree.ElementTree as ET
from collections import Counter

path, pid = sys.argv[1], sys.argv[2]
frames = int(sys.argv[3]) if len(sys.argv) > 3 else 0
ids, cols, iv = {}, None, []
for _, e in ET.iterparse(path, events=('end',)):
    if e.tag == 'schema':
        cols = [c.find('mnemonic').text for c in e.findall('col')]
        continue
    if e.tag != 'row':
        if 'id' in e.attrib:
            ids[e.attrib['id']] = e
        continue
    d = dict(zip(cols, [ids[x.attrib['ref']] if 'ref' in x.attrib else x for x in e]))
    p, lvl = d.get('process'), d.get('event-depth')
    if p is not None and f'({pid})' in p.attrib.get('fmt', '') and lvl is not None and lvl.text == '0':
        iv.append((int(d['start'].text), int(d['duration'].text), d['channel-name'].text,
                   d['cmdbuffer-id'].text, int(d['encoder-id'].text)))
    e.clear()
iv.sort()
if not iv:
    sys.exit(f'no depth-0 intervals of pid {pid}')

edges = [0, 5e3, 40e3, 160e3, 1e6, 1e15]
names = ['0-5', '5-40', '40-160', '160-1000', '>=1000']
count, idle, steps = Counter(), Counter(), {n: Counter() for n in names}
mid, kinds, kind_idle = [], Counter(), Counter()
small = lambda r: r[2] == 'Compute' and r[1] < 6e3
end, prev = iv[0][0] + iv[0][1], 0
for i in range(1, len(iv)):
    s, d, ch, cb, enc = iv[i]
    if s > end:
        g = s - end
        b = next(n for n, lo, hi in zip(names, edges, edges[1:]) if lo <= g < hi)
        count[b] += 1
        idle[b] += g
        k = enc - iv[prev][4]
        steps[b][k if -1 <= k <= 4 else ('<-1' if k < -1 else '>4')] += 1
        if b == '40-160':
            mid.append(g)
            last = i + 1 < len(iv) and iv[i + 1][3] != cb
            if small(iv[i]) and last:
                kk = 'after: small Compute, last of its command buffer (query copy at a list end)'
            elif small(iv[prev]):
                kk = f'before: small Compute, after: {"small Compute" if small(iv[i]) else ch}'
            else:
                kk = f'other: {iv[prev][2]} -> {ch}{" (small)" if small(iv[i]) else ""}'
            kinds[kk] += 1
            kind_idle[kk] += g
    if s + d > end:
        end, prev = s + d, i
win = end - iv[0][0]
tot = sum(idle.values())
per = lambda n: f'  {n / frames:7.1f}/frame' if frames else ''
print(f'pid {pid}: {len(iv)} depth-0 intervals, idle {tot / 1e6:.1f} of {win / 1e6:.1f} ms ({100 * tot / win:.1f}%)'
      + (f', {frames} frames' if frames else ''))
print('gap (us)     gaps  idle share  encoder id step across the gap')
for n in names:
    if count[n]:
        st_ = ', '.join(f'{k}: {100 * v / count[n]:.1f}%' for k, v in
                        sorted(steps[n].items(), key=lambda x: str(x[0])) if v / count[n] >= 0.005)
        print(f'{n:>9s} {count[n]:7d}  {100 * idle[n] / tot:6.1f}%    {st_}' + per(count[n]))
if mid:
    mid.sort()
    print(f'40-160 us gaps: median {st.median(mid) / 1e3:.1f} us, p10 {mid[len(mid) // 10] / 1e3:.1f}, '
          f'p90 {mid[len(mid) * 9 // 10] / 1e3:.1f}')
    for k, v in kinds.most_common():
        print(f'  {v:7d} {100 * v / len(mid):5.1f}%  {kind_idle[k] / 1e6:7.1f} ms' + per(v) +
              (f' {kind_idle[k] / 1e6 / frames:5.2f} ms/frame' if frames else '') + f'  {k}')
