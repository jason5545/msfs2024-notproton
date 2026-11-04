#!/usr/bin/env python3
# jitcat.py <jd dir>: sample share by host-instruction role in FEX's JIT code (pattern-classified).
import sys, re, collections
d = sys.argv[1]
ranges = [tuple(int(x, 16) for x in l.split()) for l in open(d + '/ranges.txt')]
def img2host(o):
    for s, e, off in ranges:
        if off <= o < off + (e - s): return s + (o - off)
ins = []
for l in open(d + '/image.dis'):
    m = re.match(r'\s*([0-9a-f]+):\s+(\S+)\s*(.*)', l)
    if m:
        h = img2host(int(m.group(1), 16))
        if h is not None: ins.append((h, m.group(2), m.group(3).strip()))
pcs = collections.Counter(); total = 0
for l in open(d + '/pcs.txt'):
    if l.startswith('#'): total = int(l.split()[-1]); continue
    a, c = l.split(); pcs[int(a, 16)] = int(c)
disp_lo = ranges[1][0]
cat = {}
n = len(ins)
for i, (h, op, args) in enumerate(ins):
    if h >= disp_lo: cat[h] = 'dispatcher'; continue
    if h in cat: continue
    nx = lambda k: ins[i + k] if i + k < n else (0, '', '')
    if op == 'adr' and args.startswith('x10,') and nx(1)[1] == 'str' and nx(1)[2] == 'x10, [x28]':
        cat[h] = cat[nx(1)[0]] = 'entry_hdr'; continue
    if op == 'ldr' and args.startswith('x11, [x28, #0x628]'):
        cat[h] = 'suspend'; cat[nx(1)[0]] = 'suspend'; cat[nx(2)[0]] = 'suspend'; continue
    if op == 'str' and args == 'x6, [x23, #-0x8]!' and nx(1)[1] == 'adr' and nx(2)[1] == 'stp' and '[x17, #-0x10]!' in nx(2)[2]:
        for k in (0, 1, 2): cat[nx(k)[0]] = 'call_push'
        cat[nx(3)[0]] = 'call_bl'
        j = i - 1
        while j >= 0 and ins[j][2].startswith('x6,') and ins[j][1] in ('mov', 'movk'):
            cat[ins[j][0]] = 'call_retaddr'; j -= 1
        continue
    if op == 'ldp' and args == 'x10, x11, [x17], #0x10':
        cat[h] = 'ret_check'
        k = 1
        while i + k < n and k < 12:
            hh, oo, aa = nx(k)
            cat[hh] = 'ret_check' if oo != 'ret' else 'ret_br'
            if oo == 'ret': break
            k += 1
        continue
    if op == 'ldr' and re.match(r'x\d+, \[x23\], #0x8', args): cat[h] = 'pop'; continue
    if op == 'cfinv': cat[h] = 'cfinv'; continue
    if op == 'mov' and re.match(r'[xw]\d+, #', args) and nx(1)[1] == 'movk':
        r = args.split(',')[0][1:]
        cat[h] = 'const_mat'
        k = 1
        while nx(k)[1] == 'movk' and nx(k)[2].split(',')[0][1:] == r: cat[nx(k)[0]] = 'const_mat'; k += 1
        continue
    if re.match(r'[xw]24,', args) and op in ('mvn', 'eor', 'mov', 'orr', 'add', 'sub'): cat[h] = 'AF'; continue
    if op == 'bfxil' and ins[i - 1][1] == 'cset': cat[h] = 'setcc_bfxil'; continue
    if op == 'cset': cat[h] = 'cset'; continue
    m = re.match(r'w(\d+), w(\d+)$', args)
    if op == 'mov' and m:
        if m.group(1) == m.group(2): cat[h] = 'zext_self'; continue
        p = ins[i - 1]
        if p[1] in ('add', 'sub', 'mov') and p[2].startswith('x%s,' % m.group(2)): cat[h] = 'lea32_zext'; continue
        cat[h] = 'mov_w'; continue
    if op == 'mov' and re.match(r'v\d+\.16b, v\d+\.16b', args): cat[h] = 'vmov'; continue
    if op == 'mov' and re.match(r'x\d+, v\d+\.d\[0\]', args): cat[h] = 'vec2gpr'; continue
    if op == 'mov' and re.match(r'x\d+, x\d+$', args): cat[h] = 'mov_x'; continue
    if op in ('ldr', 'ldur', 'ldp', 'ldrb', 'ldrh', 'ldrsw', 'ldrsb', 'ldrsh', 'ldapr', 'ldaprb'): cat[h] = 'load'; continue
    if op in ('str', 'stur', 'stp', 'strb', 'strh', 'stlr', 'stlur'): cat[h] = 'store'; continue
    if op.startswith('f') or re.search(r'\bv\d+\.', args) or re.match(r'[dsq]\d+', args): cat[h] = 'fp/vec'; continue
    if op.startswith('b') or op in ('cbz', 'cbnz', 'tbz', 'tbnz', 'ret', 'br', 'blr'): cat[h] = 'branch'; continue
    cat[h] = 'alu'
byc = collections.Counter(); jit = 0
for a, c in pcs.items():
    if a in cat: byc[cat[a]] += c; jit += c
main = total - pcs.most_common(1)[0][1]
print('main-thread samples %d, in JIT image %d (%.1f%%)' % (main, jit, 100.0 * jit / main))
for k, c in byc.most_common(): print('%-14s %6d %5.1f%%' % (k, c, 100.0 * c / main))
# static counts (unweighted) for reference
sc = collections.Counter(cat.values())
print('static:', ', '.join('%s=%d' % kv for kv in sc.most_common()))
