#!/usr/bin/env python3
# Self-time profile of FEX's translated code from macOS `sample` output: every frame of the call tree
# that falls in a block FEX registered in /tmp/perf-<pid>.map (FEX_BLOCKJITNAMING=1) is credited, by
# its self samples, to the guest address of that block; guest addresses inside our own exe resolve to
# functions with llvm-symbolizer (the exe built with -g). Only for binaries we built ourselves.
#
#   jitprof.py <sample.txt> <perf map> <exe> [top N]
import bisect, collections, re, subprocess, sys

sample, pmap, exe = sys.argv[1:4]
top = int(sys.argv[4]) if len(sys.argv) > 4 else 40
exename = exe.rsplit('/', 1)[-1].lower()

# perf map: "<host> <size hex> <name>", name "JIT_0x<guest>_<host>" or "<file>+0x<off> (<host>)"
starts, ends, names = [], [], []
for line in open(pmap, errors='replace'):
    p = line.split(' ', 2)
    if len(p) < 3:
        continue
    h, sz, nm = int(p[0], 16), int(p[1], 16), p[2].strip()
    starts.append(h); ends.append(h + sz); names.append(nm)
order = sorted(range(len(starts)), key=lambda i: starts[i])
starts = [starts[i] for i in order]; ends = [ends[i] for i in order]; names = [names[i] for i in order]


def block(addr):
    i = bisect.bisect_right(starts, addr) - 1
    if i >= 0 and addr < ends[i]:
        return names[i], addr - starts[i]
    return None, None


# sample call tree: "<indent>+ <count> <symbol>  (in <image>) ... [0xaddr]"
node = re.compile(r'^([ +!:|]*)(\d+) (.*?)(?:\s+\[(0x[0-9a-f]+)(?:,0x[0-9a-f]+)*\])?\s*$')
rows = []
in_tree = False
for line in open(sample, errors='replace'):
    if line.startswith('Call graph:'):
        in_tree = True; continue
    if in_tree and (line.startswith('Total number in stack') or line.startswith('Sort by top of stack')):
        break
    if not in_tree:
        continue
    m = node.match(line.rstrip('\n'))
    if m:
        rows.append((len(m.group(1)), int(m.group(2)), m.group(3), m.group(4)))

selfc = collections.Counter()
other = collections.Counter()
total = 0
for i, (depth, cnt, sym, addr) in enumerate(rows):
    # children are the rows at the next indentation level until we leave this subtree
    child = 0
    nxt = None
    for d2, c2, _, _ in rows[i + 1:]:
        if d2 <= depth:
            break
        if nxt is None:
            nxt = d2
        if d2 == nxt:
            child += c2
    s = cnt - child
    if s <= 0:
        continue
    total += s
    nm = None
    if addr:
        nm, off = block(int(addr, 16))
    if nm:
        selfc[nm] += s
    else:
        other[re.sub(r'\s+\+ \d+.*$', '', sym)[:90]] += s

jit = sum(selfc.values())
print('self samples: total=%d in translated code=%d (%.1f%%)' % (total, jit, 100.0 * jit / max(total, 1)))

# guest address per block -> function in our exe
guest = {}
for nm in selfc:
    m = re.match(r'(.*)\+0x([0-9a-f]+) \(', nm)
    if m and m.group(1).lower().endswith(exename):
        guest[nm] = int(m.group(2), 16)
funcs = {}
if guest:
    q = '\n'.join('0x%x' % (0x140000000 + o) for o in guest.values())
    out = subprocess.run(['/opt/homebrew/opt/llvm/bin/llvm-symbolizer', '--obj=' + exe, '--functions=linkage',
                          '--no-inlines'], input=q, capture_output=True, text=True).stdout.strip().split('\n\n')
    for (nm, _), res in zip(guest.items(), out):
        funcs[nm] = res.split('\n')[0]
byfunc = collections.Counter()
for nm, c in selfc.items():
    byfunc[funcs.get(nm, nm if nm.startswith('JIT_') or nm.startswith('Dispatch') else re.sub(r' \(.*', '', nm))] += c
print('\n-- translated code by guest function (self samples, %% of all)')
for f, c in byfunc.most_common(top):
    print('%6d %5.1f%%  %s' % (c, 100.0 * c / total, f))
print('\n-- outside translated code')
for f, c in other.most_common(15):
    print('%6d %5.1f%%  %s' % (c, 100.0 * c / total, f))
