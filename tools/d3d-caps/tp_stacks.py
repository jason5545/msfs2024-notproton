#!/usr/bin/env python3
"""tp_stacks.py <time-profile.xml> <pid> <thread-substring> [mode]
Per-thread stack attribution: leaf binary, inclusive binaries, inclusive symbols (Mach-O symbolicated), and
the first symbolicated frame above unsymbolicated (PE/JIT) leaf frames."""
import collections, re, sys
import xml.etree.ElementTree as ET
path, pid, want = sys.argv[1], sys.argv[2], sys.argv[3]
mode = sys.argv[4] if len(sys.argv) > 4 else 'all'
text = open(path).read(); end = text.find('</trace-query-result>')
root = ET.fromstring(text[:end + len('</trace-query-result>')])
ids = {e.attrib['id']: e for e in root.iter() if 'id' in e.attrib}
def V(e): return ids[e.attrib['ref']] if e is not None and 'ref' in e.attrib else e
cols = [c.findtext('mnemonic') for c in root.iter('col')]
leafbin = collections.Counter(); incbin = collections.Counter(); incsym = collections.Counter()
firstsym = collections.Counter(); chains = collections.Counter(); total = 0
for row in root.iter('row'):
    r = {k: V(v) for k, v in zip(cols, list(row))}
    th = r.get('thread'); fm = th.attrib.get('fmt', '') if th is not None else ''
    if ('pid: %s)' % pid) not in fm or want not in fm: continue
    w = int(r['weight'].text) if r.get('weight') is not None else 1000000
    total += w
    bt = r.get('stack'); frames = []
    if bt is not None:
        for f in bt.findall('frame'):
            f = V(f); b = f.find('binary'); b = V(b)
            bn = b.attrib.get('name') if b is not None else None
            nm = f.attrib.get('name', '?')
            frames.append((bn, nm, int(f.attrib.get('addr', '0'), 16)))
    if not frames: leafbin['<none>'] += w; continue
    bn, nm, a = frames[0]
    leafbin[bn or ('addr %#x' % (a >> 24 << 24))] += w
    seenb = set(); seens = set()
    for bn, nm, a in frames:
        k = bn or 'NOBIN'
        if k not in seenb: seenb.add(k); incbin[k] += w
        if bn:
            s = '%s`%s' % (bn, nm[:80])
            if s not in seens: seens.add(s); incsym[s] += w
    # first symbolicated frames from leaf (chain of up to 4 symbolicated)
    sym = [('%s`%s' % (bn, nm[:60])) for bn, nm, a in frames if bn]
    if sym: firstsym[sym[0]] += w; chains[' < '.join(sym[:5])] += w
    else: firstsym['<all unsymbolicated>'] += w
def show(title, c, n=25):
    print('\n== %s' % title)
    for k, v in c.most_common(n): print('  %5.1f%%  %s' % (100 * v / total, k[:200]))
print('thread match %r: %.0f ms of samples' % (want, total / 1e6))
show('leaf binary (unsymbolicated bucketed by 16 MiB)', leafbin)
show('inclusive binary', incbin)
show('inclusive symbol', incsym, 50)
if mode == 'all': show('symbol chains from leaf', chains, 40)
