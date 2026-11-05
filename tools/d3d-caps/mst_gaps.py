#!/usr/bin/env python3
"""GPU idle gaps of one process in a Metal System Trace (records 25.58).

Reads metal-gpu-intervals.xml (xctrace export of the GPU instrument), keeps the depth-0 intervals of <pid>,
merges overlapping ones and looks at the gaps between them:
  - size distribution, and each size bucket's share of the idle time;
  - per bucket, how often the Metal command buffer id changes across the gap (a gap between command buffers)
    rather than staying the same (a gap between encoders of one command buffer). The ids are reused over a
    trace (KosmicKrisp uses MTL4 command buffers, which are reusable objects), so an id only tells two
    neighbouring intervals apart, not command buffers over the whole trace;
  - for gaps of 40-160 us inside one command buffer: the channels before and after, and the encoder lengths.
  mst_gaps.py <metal-gpu-intervals.xml> <pid> [presents in the window]"""
import sys
import xml.etree.ElementTree as ET
from collections import Counter, defaultdict

path, pid = sys.argv[1], sys.argv[2]
presents = int(sys.argv[3]) if len(sys.argv) > 3 else 0
ids = {}
iv = []
for _, e in ET.iterparse(path, events=('end',)):
    if e.tag != 'row':
        if 'id' in e.attrib:
            ids[e.attrib['id']] = e
        continue
    vals = [ids[x.attrib['ref']] if 'ref' in x.attrib else x for x in e]
    proc = [x for x in vals if x.tag == 'process']
    lvl = [x for x in vals if x.tag == 'metal-nesting-level']
    cb = [x for x in vals if x.tag == 'metal-command-buffer-id']
    if proc and f'({pid})' in proc[0].attrib.get('fmt', '') and lvl and lvl[0].text == '0' and cb:
        s, d = int(vals[0].text), int(vals[1].text)
        iv.append((s, s + d, cb[0].text, vals[2].text, d))
    e.clear()
iv.sort()

edges = [0, 5e3, 40e3, 80e3, 160e3, 500e3, 1e6, 5e6, 1e15]
bucket = defaultdict(lambda: [0, 0, 0])  # gaps, idle ns, gaps across a command buffer change
pairs = Counter()
before, after = [], []
end, prev = iv[0][1], iv[0]
for cur in iv[1:]:
    s, t, cb, ch, d = cur
    if s > end:
        g = s - end
        for a, b in zip(edges, edges[1:]):
            if a <= g < b:
                x = bucket[(a, b)]
                x[0] += 1
                x[1] += g
                x[2] += cb != prev[2]
        if 40e3 <= g < 160e3 and cb == prev[2]:
            pairs[(prev[3], ch)] += 1
            before.append(prev[4])
            after.append(d)
    if t > end:
        end, prev = t, cur
win = end - iv[0][0]
idle = sum(x[1] for x in bucket.values())
ngaps = sum(x[0] for x in bucket.values())
print(f'pid {pid}: {len(iv)} depth-0 intervals, {ngaps} gaps, idle {idle / 1e6:.1f} ms of {win / 1e6:.1f} ms'
      f' ({100 * idle / win:.1f}%)' + (f', {presents} presents' if presents else ''))
print('gap bucket (us)      gaps  share of idle  across a command buffer change' +
      ('  gaps/frame  idle ms/frame' if presents else ''))
for a, b in zip(edges, edges[1:]):
    n, g, c = bucket[(a, b)]
    if not n:
        continue
    hi = f'{b / 1e3:.0f}' if b < 1e14 else 'inf'
    line = f'  {a / 1e3:>6.0f}-{hi:<8s} {n:7d}  {100 * g / idle:6.1f}%        {100 * c / n:5.1f}%'
    if presents:
        line += f'       {n / presents:6.1f}      {g / 1e6 / presents:6.2f}'
    print(line)
print('40-160 us gaps inside one command buffer: channel before -> after')
for (a, b), n in pairs.most_common(6):
    print(f'  {a:9s} -> {b:9s} {n}')
before.sort()
after.sort()
if before:
    print(f'  encoder before the gap: median {before[len(before) // 2] / 1e3:.1f} us;'
          f' after: median {after[len(after) // 2] / 1e3:.1f} us')
