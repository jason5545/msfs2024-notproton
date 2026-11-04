#!/usr/bin/env python3
# Self-time profile of the PE modules in a Wine process from macOS `sample` output, which shows their
# frames as ??? with an address: each address is placed in a module by the bases in a +loaddll log of
# the same process and named from that module's symbol table (llvm-nm) in a build directory we built
# ourselves (FEX's xtajit64.dll is build_arm64ec/Bin/libarm64ecfex.dll, Wine's DLLs are in its build tree).
# Mach-O frames keep the name sample gave them.
#
#   peprof.py <sample.txt> <stderr with +loaddll> <module=path>... [--top N]
import bisect, collections, re, subprocess, sys

args = sys.argv[1:]
top = 30
if '--top' in args:
    i = args.index('--top'); top = int(args[i + 1]); del args[i:i + 2]
sample, loadlog, mods = args[0], args[1], dict(a.split('=', 1) for a in args[2:])

bases = {}
for l in open(loadlog, errors='replace'):
    m = re.search(r'Loaded L"([^"]*)" at ([0-9A-Fa-f]+)', l)
    if m:
        name = m.group(1).split('\\')[-1].lower()
        bases.setdefault(name, int(m.group(2), 16))

symtabs = {}
for name, path in mods.items():
    out = subprocess.run(['/opt/homebrew/opt/llvm/bin/llvm-nm', '--demangle', path], capture_output=True, text=True).stdout
    syms = sorted((int(p[0], 16), p[2]) for p in (l.split(' ', 2) for l in out.splitlines()) if len(p) == 3 and p[1] in 'tT')
    img = int(subprocess.run(['/opt/homebrew/opt/llvm/bin/llvm-objdump', '-p', path], capture_output=True, text=True).stdout
              .split('ImageBase', 1)[1].split()[0], 16)
    symtabs[name.lower()] = (img, [s[0] for s in syms], [s[1] for s in syms])
ranges = sorted((b, n) for n, b in bases.items())

node = re.compile(r'^([ +!:|]*)(\d+) (.*?)(?:\s+\[(0x[0-9a-f]+)(?:,0x[0-9a-f]+)*\])?\s*$')
rows, f = [], 0
for l in open(sample, errors='replace'):
    if l.startswith('Call graph:'):
        f = 1; continue
    if f and l.startswith('Total number'):
        break
    if f:
        m = node.match(l.rstrip('\n'))
        if m:
            rows.append((len(m.group(1)), int(m.group(2)), m.group(3), m.group(4)))

c, total = collections.Counter(), 0
for i, (d, n, s, a) in enumerate(rows):
    ch, nx = 0, None
    for d2, n2, _, _ in rows[i + 1:]:
        if d2 <= d:
            break
        if nx is None:
            nx = d2
        if d2 == nx:
            ch += n2
    self_n = n - ch
    if self_n <= 0:
        continue
    total += self_n
    label = re.sub(r'\s+\+ \d+.*$', '', s)[:100]
    if s.startswith('???') and a:
        addr = int(a, 16)
        j = bisect.bisect_right([r[0] for r in ranges], addr) - 1
        if j >= 0 and ranges[j][1] in symtabs:
            img, ad, nm = symtabs[ranges[j][1]]
            va = addr - ranges[j][0] + img
            k = bisect.bisect_right(ad, va) - 1
            label = '%s!%s' % (ranges[j][1], nm[k][:90] if k >= 0 else hex(va))
        elif j >= 0 and addr - ranges[j][0] < 0x4000000:
            label = '%s+?' % ranges[j][1]
        else:
            label = '??? (JIT or unknown)'
    c[label] += self_n
print('self samples', total)
for k, v in c.most_common(top):
    print('%6d %5.1f%%  %s' % (v, 100.0 * v / total, k))
