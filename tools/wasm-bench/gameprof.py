#!/usr/bin/env python3
# gameprof.py <dir> : per-thread self samples of a `sample` of a Wine+FEX process, leaves placed in PE modules
# (modules.txt from vmmap; sizes below), Mach-O images (named by sample), or anonymous memory (FEX's JIT code).
import re, sys, bisect, collections
o = sys.argv[1]
size = {'ucrtbase.dll': 0x200000, 'ntdll.dll': 0x1c0000, 'kernelbase.dll': 0x3c0000, 'msvcrt.dll': 0x1a0000,
        'kernel32.dll': 0x120000, 'xtajit64.dll': 0x4bb000, 'flightsimulator2024.exe': 199314944 * 11 // 10,
        'm663c42dcb7b68d27_0.dll': 22173696 * 11 // 10, 'mf8e49e2927256992_0.dll': 3202560 * 11 // 10}
mods = []
for l in open(o + '/modules.txt'):
    s, e, p = l.rstrip('\n').split(' ', 2)
    b = p.rsplit('/', 1)[-1]
    mods.append((int(s, 16), b))
mods.sort()
starts = [m[0] for m in mods]
def pe(a):
    i = bisect.bisect_right(starts, a) - 1
    if i < 0: return None
    s, b = mods[i]
    lim = size.get(b.lower(), 0x400000)
    if i + 1 < len(mods): lim = min(lim, mods[i + 1][0] - s)
    return b if a - s < lim else None
node = re.compile(r'^([ +!:|]*)(\d+) (.*?)(?:\s+\[(0x[0-9a-f]+)(?:,0x[0-9a-f]+)*\])?\s*$')
threads = {}
cur = None; rows = None; in_tree = False
for line in open(o + '/sample.txt', errors='replace'):
    if line.startswith('Call graph:'): in_tree = True; continue
    if in_tree and (line.startswith('Total number in stack') or line.startswith('Sort by top of stack')): break
    if not in_tree: continue
    m = node.match(line.rstrip('\n'))
    if not m: continue
    depth, cnt, sym, addr = len(m.group(1)), int(m.group(2)), m.group(3), m.group(4)
    if 'Thread_' in sym and depth <= 6:
        cur = sym.split()[0]; rows = []; threads[cur] = (cnt, rows); continue
    if rows is not None: rows.append((depth, cnt, sym, addr))
IDLE = re.compile(r'mach_msg2_trap|__psynch_cvwait|semaphore_wait_trap|semaphore_timedwait_trap|__workq_kernreturn|kevent|__semwait_signal|__select|__ulock_wait|__psynch_mutexwait|swtch_pri|thread_suspend')
def leaves(rows):
    out = collections.Counter()
    for i, (d, c, sym, addr) in enumerate(rows):
        child = 0; nxt = None
        for d2, c2, _, _ in rows[i + 1:]:
            if d2 <= d: break
            if nxt is None: nxt = d2
            if d2 == nxt: child += c2
        s = c - child
        if s <= 0: continue
        if addr and sym.startswith('???'):
            a = int(addr, 16); b = pe(a)
            key = ('PE ' + b) if b else 'anon (JIT/dispatcher)'
            if b == 'xtajit64.dll': key += ' +0x%x' % (a - [m[0] for m in mods if m[1] == b][0])
        else:
            mm = re.match(r'(\S+).*?\(in ([^)]+)\)', sym)
            key = ('idle ' if (IDLE.search(sym) or 'libsystem_kernel' in sym) else '') + (mm.group(2) + ' ' + mm.group(1) if mm else sym[:60])
        out[key] += s
    return out
summ = []
for t, (cnt, rows) in threads.items():
    lv = leaves(rows)
    busy = sum(c for k, c in lv.items() if not k.startswith('idle'))
    summ.append((busy, cnt, t, lv))
summ.sort(reverse=True)
print('threads %d; busiest:' % len(summ))
for busy, cnt, t, lv in summ[:8]:
    print('  %s total=%d busy=%d' % (t, cnt, busy))
for busy, cnt, t, lv in summ[:3]:
    print('\n== %s busy %d of %d' % (t, busy, cnt))
    agg = collections.Counter()
    for k, c in lv.items():
        if k.startswith('idle'): agg['idle (waits)'] += c
        elif k.startswith('PE xtajit64'): agg['PE xtajit64.dll (FEX native: dispatcher helpers, ExitFunctionEC)'] += c
        elif k.startswith('PE '): agg[k] += c
        elif k.startswith('anon'): agg[k] += c
        else: agg['mach-o ' + k.split()[0]] += c
    for k, c in agg.most_common(14): print('  %6d %5.1f%%  %s' % (c, 100.0 * c / cnt, k))
    print('  -- top leaves')
    for k, c in lv.most_common(14): print('  %6d %5.1f%%  %s' % (c, 100.0 * c / cnt, k))
