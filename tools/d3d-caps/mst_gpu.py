#!/usr/bin/env python3
"""mst_gpu.py <trace> [pid]: GPU time per frame from an Instruments "Metal System Trace" recording.

Exports the metal-gpu-intervals table with `xctrace export`, keeps the depth-0 "GPU Execution" intervals
(of one process if a pid is given), and prints, per Metal frame number, the GPU busy time (union of the
intervals, so overlapping vertex/fragment/compute work is counted once), the span from first start to last
end, and the summed time per channel. Ends with the median and p90 of busy time over the frames.
"""
import subprocess, sys, statistics, collections
import xml.etree.ElementTree as ET

trace = sys.argv[1]
want_pid = sys.argv[2] if len(sys.argv) > 2 else None
xml = subprocess.run(['xcrun', 'xctrace', 'export', '--input', trace, '--xpath',
                      '/trace-toc/run[@number="1"]/data/table[@schema="metal-gpu-intervals"]'],
                     capture_output=True, text=True, check=True).stdout
root = ET.fromstring(xml)
ids = {}
def val(e):
    """xctrace writes a value once with id= and refers to it later with ref=."""
    if e is None: return None
    if 'ref' in e.attrib: e = ids[e.attrib['ref']]
    return e
for e in root.iter():
    if 'id' in e.attrib: ids[e.attrib['id']] = e
cols = [c.findtext('mnemonic') for c in root.iter('col')]
frames = collections.defaultdict(list)
for row in root.iter('row'):
    cells = dict(zip(cols, list(row)))
    depth = val(cells.get('event-depth'))
    if depth is None or (depth.text or '0') != '0': continue
    if want_pid:
        p = val(cells.get('process'))
        pid = p.find('pid') if p is not None else None
        pid = val(pid)
        if pid is None or pid.text != want_pid: continue
    st = int(val(cells['start']).text); du = int(val(cells['duration']).text)
    fr = val(cells.get('frame-number')); fr = int(fr.text) if fr is not None and fr.text else -1
    ch = val(cells.get('channel-name')); ch = ch.attrib.get('fmt', ch.text) if ch is not None else '?'
    frames[fr].append((st, st + du, ch))
busy_all = []
print('frame  busy_ms  span_ms  per-channel_ms')
for fr in sorted(frames):
    iv = sorted(frames[fr])
    busy = 0; cs, ce = iv[0][0], iv[0][1]
    for s, e, _ in iv[1:]:
        if s > ce: busy += ce - cs; cs, ce = s, e
        else: ce = max(ce, e)
    busy += ce - cs
    span = max(e for _, e, _ in iv) - iv[0][0]
    per = collections.Counter()
    for s, e, ch in iv: per[ch] += e - s
    if fr >= 0: busy_all.append(busy / 1e6)  # -1: intervals Metal did not assign to a presented frame
    print('%5d %8.3f %8.3f  %s' % (fr, busy / 1e6, span / 1e6,
                                   ' '.join('%s=%.3f' % (k, v / 1e6) for k, v in per.most_common())))
if busy_all:
    q = sorted(busy_all)
    print('frames %d  busy median %.3f ms  p90 %.3f ms  max %.3f ms' %
          (len(q), statistics.median(q), q[int(len(q) * 0.9) - 1 if len(q) > 1 else 0], q[-1]))
