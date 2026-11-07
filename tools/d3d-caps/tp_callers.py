#!/usr/bin/env python3
"""tp_callers.py <xml> <pid> <thread-substr> <symbol-substr>: for samples whose stack contains a symbolicated frame
matching symbol-substr, show caller chains (frames above it, outward) and callee leaf."""
import collections, sys
import xml.etree.ElementTree as ET
path, pid, want, sym = sys.argv[1:5]
text = open(path).read(); end = text.find('</trace-query-result>')
root = ET.fromstring(text[:end + len('</trace-query-result>')])
ids = {e.attrib['id']: e for e in root.iter() if 'id' in e.attrib}
def V(e): return ids[e.attrib['ref']] if e is not None and 'ref' in e.attrib else e
cols = [c.findtext('mnemonic') for c in root.iter('col')]
up = collections.Counter(); total = 0; hit = 0
for row in root.iter('row'):
    r = {k: V(v) for k, v in zip(cols, list(row))}
    th = r.get('thread'); fm = th.attrib.get('fmt', '') if th is not None else ''
    if ('pid: %s)' % pid) not in fm or want not in fm: continue
    w = int(r['weight'].text); total += w
    bt = r.get('stack'); fr = []
    if bt is None: continue
    for f in bt.findall('frame'):
        f = V(f); b = V(f.find('binary'))
        fr.append(f.attrib.get('name', '?') if b is not None else 'PE/JIT')
    idx = [i for i, n in enumerate(fr) if sym in n]
    if not idx: continue
    hit += w; i = idx[-1]
    chain = []
    for n in fr[i:i + 7]:
        if chain and chain[-1] == n: continue
        chain.append(n[:45])
    up[' > '.join(reversed(chain))] += w
print('%s in %s: %.1f%% of thread (%.0f ms)' % (sym, want, 100 * hit / total, hit / 1e6))
for k, v in up.most_common(int(sys.argv[5]) if len(sys.argv) > 5 else 15): print('  %5.1f%%  %s' % (100 * v / total, k))
