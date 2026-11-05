#!/usr/bin/env python3
"""tp_threads.py <trace> <pid> [top] [xmldir]: CPU per thread from an Instruments Time Profiler recording.

Unlike `sample`, Time Profiler takes kernel (kperf) samples of the threads that are on a core and does not suspend the
program, so a sample means the thread really ran. Prints, for <pid>: the FPS from the present requests (when the
recording also had the Metal Application instrument), CPU per thread as a share of one core, the core class it ran on,
and for the busiest threads the most frequent leaf frames (symbol if Instruments resolved one, else module-less
address bucketed by 1 MiB, which is what translated or JIT code looks like).

Record with
  xcrun xctrace record --template 'Time Profiler' --instrument 'Metal Application' --instrument Display \
       --attach <pid> --time-limit 5s --output x.trace
Tables are exported once into xmldir (default <trace>-xml next to the trace).
"""
import collections, os, re, statistics, subprocess, sys
import xml.etree.ElementTree as ET

trace, want_pid = sys.argv[1], sys.argv[2]
top = int(sys.argv[3]) if len(sys.argv) > 3 else 25
xmldir = sys.argv[4] if len(sys.argv) > 4 else trace.rstrip('/').removesuffix('.trace') + '-xml'
os.makedirs(xmldir, exist_ok=True)


def table(schema):
    path = os.path.join(xmldir, schema + '.xml')
    if not os.path.exists(path):
        xml = subprocess.run(['xcrun', 'xctrace', 'export', '--input', trace, '--xpath',
                              '/trace-toc/run[@number="1"]/data/table[@schema="%s"]' % schema],
                             capture_output=True, text=True, check=True).stdout
        with open(path, 'w') as f: f.write(xml)
    text = open(path).read()
    end = text.find('</trace-query-result>')  # xctrace sometimes appends a second document
    root = ET.fromstring(text[:end + len('</trace-query-result>')] if end >= 0 else text)
    ids = {e.attrib['id']: e for e in root.iter() if 'id' in e.attrib}
    cols = [c.findtext('mnemonic') for c in root.iter('col')]

    def val(e):
        if e is None: return None
        return ids[e.attrib['ref']] if 'ref' in e.attrib else e

    for row in root.iter('row'):
        yield {k: val(v) for k, v in zip(cols, list(row))}, val


def fmt(e): return (e.attrib.get('fmt') or e.text or '?') if e is not None else '?'
def pid_in(name): m = re.search(r'pid: (\d+)\)|\((\d+)\)$', name); return m and (m.group(1) or m.group(2))


pres = []
try:
    for r, _ in table('ca-client-present-request'):
        if pid_in(fmt(r.get('process'))) == want_pid: pres.append(int(r['timestamp'].text))
except subprocess.CalledProcessError:
    pass
pres.sort()

cpu = collections.Counter(); cores = collections.defaultdict(collections.Counter)
leaves = collections.defaultdict(collections.Counter); states = collections.Counter()
t_first = t_last = None; total = 0
for r, val in table('time-profile'):
    th = fmt(r.get('thread'))
    if pid_in(th) != want_pid: continue
    t = int(r['time'].text); w = int(r['weight'].text) if r.get('weight') is not None else 1000000
    t_first = t if t_first is None else min(t_first, t); t_last = t if t_last is None else max(t_last, t)
    name = re.sub(r' \(0x[0-9a-f]+\) \(wine, pid: \d+\)$', '', th)
    tid = re.search(r'\((0x[0-9a-f]+)\)', th); key = '%s [%s]' % (name, tid.group(1) if tid else '?')
    cpu[key] += w; total += w
    core = fmt(r.get('core')); cores[key][re.sub(r'CPU \d+ ', '', core)] += w
    states[fmt(r.get('thread-state'))] += w
    bt = r.get('stack')
    fr = bt.find('frame') if bt is not None else None
    if fr is not None:
        fr = val(fr)
        nm = fr.attrib.get('name', '?')
        if re.fullmatch(r'0x[0-9a-f]+', nm): nm = 'addr %s' % hex(int(nm, 16) >> 20 << 20)
        leaves[key][nm] += w

span = (t_last - t_first) / 1e9
if len(pres) > 2:
    g = sorted((b - a) / 1e6 for a, b in zip(pres, pres[1:]))
    print('FPS %.2f over %.2f s (%d presents)  interval median %.2f ms  p90 %.2f  max %.2f' %
          ((len(pres) - 1) / ((pres[-1] - pres[0]) / 1e9), (pres[-1] - pres[0]) / 1e9, len(pres),
           statistics.median(g), g[int(len(g) * 0.9)], g[-1]))
print('samples span %.2f s; process CPU %.2f cores; thread states %s' %
      (span, total / 1e9 / span, dict((k, round(v / total * 100, 1)) for k, v in states.items())))
print('\n%-48s %7s  %s' % ('thread', 'core%', 'core class'))
for k, w in cpu.most_common(top):
    cc = ' '.join('%s %.0f%%' % (c, 100 * v / w) for c, v in cores[k].most_common())
    print('%-48s %6.1f%%  %s' % (k[:48], 100 * w / 1e9 / span, cc))
print('\nleaf frames of the busiest threads:')
for k, w in cpu.most_common(6):
    print('  %s' % k)
    for nm, v in leaves[k].most_common(6):
        print('      %5.1f%%  %s' % (100 * v / w, nm[:90]))
