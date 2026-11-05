#!/usr/bin/env python3
"""mst_game.py <trace> <pid> [xmldir]: GPU time per presented frame for a program whose GPU work Metal does not
assign to frames (KosmicKrisp blits the finished image into the drawable, so only that blit gets a frame number and
mst_gpu.py puts nearly everything under frame -1).

Instead of per-frame numbers it uses the presents as the clock: frames = present requests of <pid>, GPU busy = union
of <pid>'s depth-0 "GPU Execution" intervals between the first and the last present, GPU ms per frame = busy / frames.

Needs a recording with the GPU, Metal Application and Display instruments, e.g.
  xcrun xctrace record --instrument GPU --instrument 'Metal Application' --instrument Display \
       --attach <pid> --time-limit 5s --output x.trace
(the full 'Metal System Trace' template crashed in XRMTLResidencySetModeler on MSFS, 11/5).

Tables are exported once into xmldir (default <trace>-xml next to the trace) and reused on later runs.
"""
import collections, os, statistics, subprocess, sys
import xml.etree.ElementTree as ET

trace, want_pid = sys.argv[1], sys.argv[2]
xmldir = sys.argv[3] if len(sys.argv) > 3 else trace.rstrip('/').removesuffix('.trace') + '-xml'
os.makedirs(xmldir, exist_ok=True)


def table(schema):
    path = os.path.join(xmldir, schema + '.xml')
    if not os.path.exists(path):
        xml = subprocess.run(['xcrun', 'xctrace', 'export', '--input', trace, '--xpath',
                              '/trace-toc/run[@number="1"]/data/table[@schema="%s"]' % schema],
                             capture_output=True, text=True, check=True).stdout
        with open(path, 'w') as f: f.write(xml)
    root = ET.parse(path).getroot()
    ids = {e.attrib['id']: e for e in root.iter() if 'id' in e.attrib}
    cols = [c.findtext('mnemonic') for c in root.iter('col')]

    def val(e):
        """xctrace writes a value once with id= and refers to it later with ref=."""
        if e is None: return None
        return ids[e.attrib['ref']] if 'ref' in e.attrib else e

    for row in root.iter('row'):
        yield {k: val(v) for k, v in zip(cols, list(row))}


def num(e): return int(e.text) if e is not None and e.text else None
def fmt(e): return (e.attrib.get('fmt') or e.text or '?') if e is not None else '?'


def union(iv):
    """Total length of the union of (start, end) intervals."""
    tot = 0; cs = ce = None
    for s, e in sorted(iv):
        if ce is None or s > ce:
            if ce is not None: tot += ce - cs
            cs, ce = s, e
        else: ce = max(ce, e)
    return tot + (ce - cs if ce is not None else 0)


def clip(iv, a, b): return [(max(s, a), min(e, b)) for s, e in iv if e > a and s < b]


def pct(q, p): return q[min(len(q) - 1, int(len(q) * p))]


# ---- presents: the frame clock
pres = []
for r in table('ca-client-present-request'):
    p = r.get('process')
    pid = p.findtext('.//pid') if p is not None else None
    if pid is None and p is not None:  # pid given by ref inside the process element
        pid = fmt(p).rsplit('(', 1)[-1].rstrip(')')
    if pid == want_pid: pres.append(num(r['timestamp']))
pres.sort()
if len(pres) < 3: sys.exit('only %d presents for pid %s' % (len(pres), want_pid))
t0, t1 = pres[0], pres[-1]
win = t1 - t0; nfr = len(pres) - 1
gaps = sorted((b - a) / 1e6 for a, b in zip(pres, pres[1:]))
print('window %.3f s between the first and last of %d presents (pid %s)' % (win / 1e9, len(pres), want_pid))
print('FPS %.2f   present interval median %.2f ms  p90 %.2f  max %.2f  min %.2f' %
      (nfr / (win / 1e9), statistics.median(gaps), pct(gaps, 0.9), gaps[-1], gaps[0]))

# ---- GPU intervals
own = collections.defaultdict(list); other = collections.defaultdict(list)
enc = []; depth_count = collections.Counter(); lat = []
for r in table('metal-gpu-intervals'):
    d = r.get('event-depth')
    depth = d.text if d is not None else '?'
    p = r.get('process')
    name = fmt(p)
    pid = name.rsplit('(', 1)[-1].rstrip(')') if '(' in name else name
    depth_count[(name, depth)] += 1
    if depth != '0': continue
    s = num(r['start']); e = s + num(r['duration']); ch = fmt(r.get('channel-name'))
    if pid == want_pid:
        own[ch].append((s, e))
        if t0 <= s < t1:
            enc.append((e - s, s, ch))
            sl = num(r.get('start-latency'))
            if sl is not None: lat.append(sl / 1e6)
    else:
        other[name].append((s, e))

allown = [x for v in own.values() for x in v]
busy = union(clip(allown, t0, t1))
print('\nGPU busy (union of depth-0 intervals of pid %s): %.1f ms of %.1f ms = %.1f%%' %
      (want_pid, busy / 1e6, win / 1e6, 100 * busy / win))
print('GPU ms per presented frame %.2f   (frame interval %.2f ms)' % (busy / 1e6 / nfr, win / 1e6 / nfr))
for ch in sorted(own):
    c = clip(own[ch], t0, t1)
    print('  %-9s union %8.1f ms  sum %8.1f ms  intervals %d' %
          (ch, union(c) / 1e6, sum(e - s for s, e in c) / 1e6, len(c)))
print('  channels summed / union = %.2f (above 1 means vertex, fragment and compute overlap)' %
      (sum(sum(e - s for s, e in clip(v, t0, t1)) for v in own.values()) / busy))

print('\nper second (from the first present): busy %')
sec = int(1e9)
for k in range(int(win // sec) + 1):
    a = t0 + k * sec; b = min(t1, a + sec)
    if b - a < sec / 4: break
    n = sum(1 for t in pres if a <= t < b)
    print('  %d  %5.1f%%  presents %d' % (k, 100 * union(clip(allown, a, b)) / (b - a), n))

print('\nother processes on the GPU in the window (union ms, %):')
for name, v in sorted(other.items(), key=lambda kv: -union(clip(kv[1], t0, t1))):
    u = union(clip(v, t0, t1))
    if u > 0: print('  %-40s %8.1f ms  %5.1f%%' % (name, u / 1e6, 100 * u / win))
everything = allown + [x for v in other.values() for x in v]
print('  all processes together: %.1f%%' % (100 * union(clip(everything, t0, t1)) / win))

d = sorted(x[0] / 1e6 for x in enc)
print('\nencoders of pid %s in the window: %d (%.0f per frame)  duration median %.3f ms  p90 %.3f  p99 %.3f  max %.3f' %
      (want_pid, len(d), len(d) / nfr, statistics.median(d), pct(d, 0.9), pct(d, 0.99), d[-1]))
for lim in (0.1, 0.5, 1, 2, 5):
    sel = [x for x in d if x >= lim]
    print('  >= %4.1f ms: %6d encoders, %8.1f ms summed (%.0f%% of the summed time)' %
          (lim, len(sel), sum(sel), 100 * sum(sel) / sum(d)))
print('  longest:')
for du, s, ch in sorted(enc, reverse=True)[:10]:
    print('    %7.3f ms  %-8s at +%.3f s' % (du / 1e6, ch, (s - t0) / 1e9))
if lat:
    lat.sort()
    print('CPU to GPU latency median %.3f ms  p90 %.3f  max %.3f' % (statistics.median(lat), pct(lat, 0.9), lat[-1]))

# ---- GPU performance state (clock)
st = collections.Counter(); induced = collections.Counter()
for r in table('gpu-performance-state-intervals'):
    s = num(r['start']); e = s + num(r['duration'])
    c = clip([(s, e)], t0, t1)
    if not c: continue
    name = fmt(r.get('gpu-performance-state'))
    st[name] += c[0][1] - c[0][0]
    if fmt(r.get('is-induced')) == 'Yes': induced[name] += c[0][1] - c[0][0]
tot = sum(st.values())
if tot:
    print('\nGPU performance state in the window:')
    for k, v in st.most_common():
        print('  %-8s %6.1f%%  (induced %.1f%%)' % (k, 100 * v / tot, 100 * induced[k] / tot))

print('\ndepth counts (process, depth): ' + ', '.join('%s d%s=%d' % (n, dp, c) for (n, dp), c in depth_count.most_common(8)))
