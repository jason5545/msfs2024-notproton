#!/usr/bin/env python3
"""tp_sum.py <time-profile.xml> <pid> <symbol-substr>...: over the whole process of a Time Profiler export
(tp_threads.py writes it), ms of samples whose stack holds a frame matching each symbol, how much of it has an
__kdebug_trace call at or below that frame (the Metal Application instrument's own cost), and the top threads."""
import collections, sys
import xml.etree.ElementTree as ET
path, pid = sys.argv[1], sys.argv[2]; syms = sys.argv[3:]
text = open(path).read(); end = text.find('</trace-query-result>')
root = ET.fromstring(text[:end + len('</trace-query-result>')])
ids = {e.attrib['id']: e for e in root.iter() if 'id' in e.attrib}
def V(e): return ids[e.attrib['ref']] if e is not None and 'ref' in e.attrib else e
cols = [c.findtext('mnemonic') for c in root.iter('col')]
tot = collections.Counter(); kd = collections.Counter(); per = collections.defaultdict(collections.Counter); all_=0
for row in root.iter('row'):
    r = {k: V(v) for k, v in zip(cols, list(row))}
    th = r.get('thread'); fm = th.attrib.get('fmt', '') if th is not None else ''
    if ('pid: %s)' % pid) not in fm: continue
    w = int(r['weight'].text); all_ += w
    bt = r.get('stack'); names = []
    if bt is not None:
        for f in bt.findall('frame'):
            f = V(f); names.append(f.attrib.get('name', '?'))
    tname = fm.split(' (0x')[0]
    for s in syms:
        hit = [i for i, n in enumerate(names) if s in n]
        if hit:
            tot[s] += w; per[s][tname] += w
            if any('__kdebug_trace' in n for n in names[:hit[0]+1]): kd[s] += w
print('process samples %.0f ms' % (all_/1e6))
for s in syms:
    print('%-45s %7.0f ms  (of which leaf-side __kdebug_trace %4.0f ms)  ' % (s, tot[s]/1e6, kd[s]/1e6) +
          ', '.join('%s %.0f' % (k[:22], v/1e6) for k, v in per[s].most_common(4)))
