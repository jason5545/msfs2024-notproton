#!/usr/bin/env python3
"""waitlog.py <recording.json> [options]: tables from a recording of Wine patch 0061 (the wait recorder).

  per thread: CPU (Mach thread times), blocked time by path, yields, time neither on the CPU nor in a recorded wait
  for the focus thread: who ended each of its waits (thread + wineserver request or ntdll lock), what it waited on

  --focus TID|busiest|first   thread to rank (default busiest: most CPU in the window)
  --labels FILE               lines "ROLE <name> tid=<n>" (waittest.exe output) or "<tid> <name>" to name threads
  --top N                     rows in the rankings (default 15)
  --threads N                 rows in the per-thread table (default 25, by CPU)
  --check-roles FILE          waittest.exe roles: check every role got the expected answer (exit 1 if not)
  --events                    print the focus thread's events (time, kind, ms, object, waker)

Times are clipped to the window [t0, t1]: the recording starts warmup seconds earlier so that waits begun before it
are not counted half (a wait that began before recording started is not in the file at all)."""
import json, os, struct, sys, collections, argparse

KINDS = {1: 'server_wait', 2: 'signal_and_wait', 3: 'keyed_wait', 4: 'keyed_release', 5: 'sleep', 6: 'sleep_alertable',
         7: 'yield', 8: 'alert_wait', 9: 'alert_send', 10: 'set_event', 11: 'pulse_event', 12: 'release_semaphore',
         13: 'release_mutant', 14: 'io_completion'}
BLOCKING = {1, 2, 3, 4, 5, 6, 8, 14}
SIGNALS = {10, 11, 12, 13}
HINTS = {0: 'NtWaitForAlertByThreadId', 1: 'WaitOnAddress', 2: 'critical section', 3: 'SRW exclusive', 4: 'SRW shared',
         5: 'condition variable', 6: 'barrier'}
F_COUNT, F_ALL, F_ALERTABLE, F_TIMEOUT, F_POLL, F_NOBLOCK, F_OPEN = 0x7f, 0x80, 0x100, 0x200, 0x400, 0x800, 0x1000
F_INPROC = 0x2000  # Wine 0067: done in process by msync (0064), no wineserver request, so no waker
STATUS = {0: 'ok', 0x101: 'alerted', 0x102: 'timeout', 0x103: 'pending', 0xc0: 'user_apc', 0x80: 'abandoned'}
EV = struct.Struct('<QQQIIIHH')


def load_requests():
    p = os.path.join(os.path.dirname(os.path.abspath(__file__)), 'requests-wine-11.18.txt')
    try:
        return [l.strip() for l in open(p)]
    except OSError:
        return []


REQ = load_requests()


def path_of(kind, obj):
    if kind in (1, 2, 14): return 'server'
    if kind in (3, 4): return 'keyed event'
    if kind == 6: return 'sleep (alertable)'
    if kind == 5: return 'sleep'
    if kind == 8: return 'alert: ' + HINTS.get(obj >> 56, 'tag %d' % (obj >> 56))
    return KINDS.get(kind, str(kind))


def waker_of(kind, w):
    """(tid or None, how) for the thread or reason that ended a wait"""
    if kind == 8:
        return (w, 'alert') if w else (None, 'no alerter (timeout, or alerted before the window)')
    if kind in (5,):
        return (None, 'timer')
    if w == 0: return (None, 'unknown (no Squark wineserver)')
    if w == 1: return (None, 'already signaled when it called')
    if w == 0x80000000: return (None, 'wineserver: timeout, timer or fd')
    if w & 0x80000000:
        req = ((w >> 21) & 0x3ff) - 1
        tid = (w & 0x1fffff) << 2
        name = REQ[req] if 0 <= req < len(REQ) else 'request %d' % req
        return (tid, name)
    return (None, '0x%x' % w)


def pct(v, tot):
    return 100.0 * v / tot if tot else 0.0


def quant(xs, q):
    if not xs: return 0.0
    xs = sorted(xs)
    return xs[min(len(xs) - 1, int(q * len(xs)))]


def main():
    ap = argparse.ArgumentParser(add_help=True, description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('json')
    ap.add_argument('--focus', default='busiest')
    ap.add_argument('--labels')
    ap.add_argument('--top', type=int, default=15)
    ap.add_argument('--threads', type=int, default=25)
    ap.add_argument('--check-roles')
    ap.add_argument('--events', action='store_true')
    a = ap.parse_args()

    meta = json.load(open(a.json))
    if 'error' in meta:
        print('recording failed:', meta['error']); return 2
    numer, denom = meta['timebase']
    ns = lambda ticks: ticks * numer / denom
    t0, t1 = meta['t0'], meta['t1']
    win_ms = ns(t1 - t0) / 1e6
    raw = open(a.json[:-5] + '.events', 'rb').read()
    events = []
    for off in range(0, len(raw) - EV.size + 1, EV.size):
        s, e, obj, tid, waker, status, kind, flags = EV.unpack_from(raw, off)
        if kind: events.append((s, e, obj, tid, waker, status, kind, flags))
    torn = meta['events'] - len(events)

    labels = {}
    if a.labels or a.check_roles:
        for l in open(a.labels or a.check_roles, errors='replace'):
            w = l.split()
            if len(w) >= 3 and w[0] == 'ROLE' and w[2].startswith('tid='): labels[int(w[2][4:])] = w[1]
            elif len(w) >= 2 and w[0] == 'MAIN' and w[1].startswith('tid='): labels[int(w[1][4:])] = 'main'
            elif len(w) == 2 and w[0].isdigit(): labels[int(w[0])] = w[1]
    names = {t['tid']: t['name'] for t in meta['threads'] if t['name']}
    order = {t['tid']: t['order'] for t in meta['threads']}
    def tname(tid):
        if tid is None: return '-'
        n = labels.get(tid) or names.get(tid) or ''
        return '%d%s' % (tid, (' ' + n) if n else '')

    # CPU in the window, by Mach thread
    mach_by_port = {m['port']: m for m in meta['mach']}
    mach_by_id = {m['mach_id']: m for m in meta['mach']}
    logged = {l['tid']: l for l in meta['logged']}
    cpu = {}
    for t in meta['threads']:
        m = None
        if t['tid'] in logged: m = mach_by_id.get(logged[t['tid']]['mach_id'])
        if m is None: m = mach_by_port.get(t['port'])
        if m is not None and m['cpu0_us'] >= 0: cpu[t['tid']] = (m['cpu1_us'] - m['cpu0_us']) / 1000.0
    for tid, l in logged.items():
        if tid not in cpu and l['mach_id'] in mach_by_id and mach_by_id[l['mach_id']]['cpu0_us'] >= 0:
            m = mach_by_id[l['mach_id']]; cpu[tid] = (m['cpu1_us'] - m['cpu0_us']) / 1000.0
    objs = {int(o['handle'], 16): (o['type'] or '?') + ((' ' + o['name']) if o['name'] else '') for o in meta['objects']}

    def clip(s, e):
        return max(0.0, ns(min(e, t1) - max(s, t0)) / 1e6)

    per = collections.defaultdict(lambda: collections.Counter())
    nwait = collections.Counter()
    for s, e, obj, tid, waker, status, kind, flags in events:
        d = clip(s, e)
        if kind in BLOCKING:
            per[tid]['wait'] += d; per[tid]['path:' + path_of(kind, obj)] += d
            if d > 0: nwait[tid] += 1
        elif kind == 7:
            per[tid]['yield'] += d; per[tid]['yields'] += obj
        elif kind in SIGNALS:
            per[tid]['signal'] += d
            if t0 <= s < t1: per[tid]['signals'] += 1
        elif kind == 9:
            if t0 <= s < t1: per[tid]['alerts_sent'] += 1

    print('recording %s: %s pid %d, window %.1f ms (warmup %.2f s), %d events (%d appended, capacity %d%s), '
          'open at the end: %d waits, %d yield runs, names %s' % (
              meta['id'], meta['exe'], meta['pid'], win_ms, meta['warmup'], len(events), meta['appended'],
              meta['capacity'], ', %d torn' % torn if torn else '', meta['open_waits'], meta['open_yields'],
              'gathered' if meta['gathered'] else 'NOT gathered'))
    if meta['appended'] > meta['capacity']:
        print('  !! the buffer filled: %d events lost after %d' % (meta['appended'] - meta['capacity'], meta['capacity']))

    tids = set(cpu) | set(per)
    rows = sorted(tids, key=lambda t: -(cpu.get(t, 0)))
    paths = collections.Counter()
    for t in tids:
        for k, v in per[t].items():
            if k.startswith('path:'): paths[k[5:]] += v
    cols = [p for p, _ in paths.most_common(6)]
    print('\nper thread (ms in the %.0f ms window; "other" = neither on the CPU nor in a recorded wait: an unrecorded '
          'wait, e.g. in a Vulkan driver, or not scheduled)' % win_ms)
    hdr = '  %-22s %8s %6s %8s %6s' % ('thread', 'cpu', 'cpu%', 'wait', 'wait%') + ''.join(' %14s' % c[:14] for c in cols) + \
          ' %8s %8s %7s %8s' % ('yield', 'signal', 'waits', 'other')
    print(hdr)
    for t in rows[:a.threads]:
        c = cpu.get(t); p = per[t]
        other = win_ms - (c or 0) - p['wait'] - (p['yield'] if c is None else 0)
        print('  %-22s %8s %6s %8.1f %6.1f' % (tname(t)[:22], '%.1f' % c if c is not None else '?',
                                              '%.1f' % pct(c, win_ms) if c is not None else '?', p['wait'], pct(p['wait'], win_ms)) +
              ''.join(' %14.1f' % p['path:' + col] for col in cols) +
              ' %8.1f %8.1f %7d %8.1f' % (p['yield'], p['signal'], nwait[t], other if c is not None else float('nan')))
    if len(rows) > a.threads: print('  ... %d more threads' % (len(rows) - a.threads))
    unmapped = [m for m in meta['mach'] if m['port'] not in {t['port'] for t in meta['threads']} and m['cpu0_us'] >= 0]
    if unmapped:
        um = sum((m['cpu1_us'] - m['cpu0_us']) / 1000.0 for m in unmapped)
        print('  (%d Mach threads without a Windows thread: %.1f ms CPU in all, e.g. the Cocoa main thread, the recorder)' % (len(unmapped), um))

    # focus thread
    if a.focus == 'busiest': focus = rows[0] if rows else None
    elif a.focus == 'first': focus = min(order, key=order.get) if order else None
    else:
        focus = next((t for t, n in labels.items() if n == a.focus), None)
        if focus is None: focus = int(a.focus, 0)
    if focus is None: return 0
    mine = [ev for ev in events if ev[3] == focus and ev[6] in BLOCKING and clip(ev[0], ev[1]) > 0]
    print('\nfocus thread %s: %d waits, %.1f ms (%.1f%% of the window), cpu %s ms' % (
        tname(focus), len(mine), per[focus]['wait'], pct(per[focus]['wait'], win_ms),
        '%.1f' % cpu[focus] if focus in cpu else '?'))
    by = collections.defaultdict(list)
    for s, e, obj, tid, waker, status, kind, flags in mine:
        wt, how = (None, 'still waiting when the window ended') if flags & F_OPEN else \
            (None, 'msync: waker not recorded') if flags & F_INPROC else waker_of(kind, waker)
        if status == 0x102: how = 'timeout'
        key = (path_of(kind, obj) if not flags & F_INPROC else 'msync', tname(wt), how)
        by[key].append(clip(s, e))
    print('  who ended its waits (by total time):')
    print('  %-32s %-24s %-30s %7s %9s %8s %8s %8s %8s' % ('path', 'waker', 'how', 'count', 'total ms', 'mean', 'p50', 'p95', 'max'))
    for (path, w, how), ds in sorted(by.items(), key=lambda kv: -sum(kv[1]))[:a.top]:
        print('  %-32s %-24s %-30s %7d %9.2f %8.3f %8.3f %8.3f %8.3f' % (path[:32], w[:24], how[:30], len(ds), sum(ds),
                                                                     sum(ds) / len(ds), quant(ds, .5), quant(ds, .95), max(ds)))
    byobj = collections.defaultdict(list)
    for s, e, obj, tid, waker, status, kind, flags in mine:
        if kind == 8: o = '%s 0x%x' % (HINTS.get(obj >> 56, '?'), obj & ((1 << 56) - 1))
        elif kind in (5, 6): o = 'timeout %.3f ms' % (-(obj - (1 << 64) if obj >= 1 << 63 else obj) / 1e4)
        elif kind in (3, 4): o = 'key 0x%x' % obj
        else: o = '0x%x %s' % (obj, objs.get(obj, '')) + (' (+%d more handles%s)' % ((flags & F_COUNT) - 1, ', all' if flags & F_ALL else '') if (flags & F_COUNT) > 1 else '')
        byobj[o].append(clip(s, e))
    print('  what it waited on:')
    for o, ds in sorted(byobj.items(), key=lambda kv: -sum(kv[1]))[:a.top]:
        print('    %9.2f ms %6d x  %s' % (sum(ds), len(ds), o))

    # alert latency: the waker's NtAlertThreadByThreadId to the end of the wait
    sends = collections.defaultdict(list)
    for s, e, obj, tid, waker, status, kind, flags in events:
        if kind == 9: sends[(tid, obj)].append(s)
    for v in sends.values(): v.sort()
    lat = []
    import bisect
    for s, e, obj, tid, waker, status, kind, flags in mine:
        if kind == 8 and waker and not flags & F_NOBLOCK:
            v = sends.get((waker, focus))
            if v:
                i = bisect.bisect_right(v, e) - 1
                if i >= 0 and v[i] >= s: lat.append(ns(e - v[i]) / 1e3)
    if lat:
        print('  alert to running again (us): n=%d p50 %.1f p95 %.1f max %.1f' % (len(lat), quant(lat, .5), quant(lat, .95), max(lat)))

    if a.events:
        print('\nfocus thread events:')
        for s, e, obj, tid, waker, status, kind, flags in sorted(ev for ev in events if ev[3] == focus):
            wt, how = ((None, 'msync') if flags & F_INPROC else waker_of(kind, waker)) if kind in BLOCKING else (None, 'msync' if flags & F_INPROC else '')
            print('  %10.3f %-16s %9.3f ms obj=0x%x waker=%s %s status=%s flags=0x%x' % (
                ns(s - t0) / 1e6, KINDS.get(kind, kind), ns(e - s) / 1e6, obj, tname(wt), how,
                STATUS.get(status, '0x%x' % status), flags))

    if a.check_roles:
        return check_roles(a.check_roles, events, labels, cpu, per, win_ms, clip)
    return 0


def check_roles(path, events, labels, cpu, per, win_ms, clip):
    """waittest.exe roles: each role's waits must have the waker and path the program was written to have"""
    tid = {n: t for t, n in labels.items()}
    period = 10
    for l in open(path, errors='replace'):
        if l.startswith('MAIN '):
            for w in l.split():
                if w.startswith('period_ms='): period = int(w[10:])
    ok = True
    def stat(role, kinds=BLOCKING):
        mine = [ev for ev in events if ev[3] == tid[role] and ev[6] in kinds and clip(ev[0], ev[1]) > 0]
        wk = collections.Counter()
        for s, e, obj, t, waker, status, kind, flags in mine:
            if status == 0x102 or flags & F_OPEN: continue  # a timeout has no waker, a wait open at the end none yet
            if flags & F_INPROC: wk[(None, 'msync', 'msync')] += 1; continue  # no waker to check
            w, how = waker_of(kind, waker)
            wk[(labels.get(w, w), how, path_of(kind, obj))] += 1
        return mine, wk
    def report(role, cond, text):
        nonlocal ok
        ok &= cond
        print('  %-4s %s  %s' % ('PASS' if cond else 'FAIL', role, text))
    print('\ncheck (waittest.exe roles, period %d ms, window %.0f ms):' % (period, win_ms))
    expected = per[tid['B']]['signals']  # one SetEvent per round of B (Sleep(period) takes a bit longer than period)
    for role, waker, how, path, n in [('A', 'B', 'event_op', 'server', expected),
                                      ('V', 'B', 'alert', 'alert: condition variable', expected / 4),
                                      ('C1', 'C2', 'alert', 'alert: critical section', None),
                                      ('C2', 'C1', 'alert', 'alert: critical section', None),
                                      ('S1', 'S2', 'alert', 'alert: SRW exclusive', None),
                                      ('S2', 'S1', 'alert', 'alert: SRW exclusive', None)]:
        mine, wk = stat(role)
        tot = sum(wk.values())
        if path == 'server' and wk[(None, 'msync', 'msync')] > tot / 2:
            # msync (Wine 0064/0067): the event wait is done in process, recorded without a waker
            waker, how, path = None, 'msync', 'msync'
        good = wk[(waker, how, path)]
        frac = good / tot if tot else 0
        cnt_ok = n is None or abs(tot - n) <= max(3, 0.1 * n)
        report(role, frac >= 0.95 and cnt_ok and tot > 0,
               '%d waits, %d (%.1f%%) ended by %s via %s on "%s"%s; others %s' % (
                   tot, good, 100 * frac, waker, how, path, '' if n is None else ' (expected about %.0f)' % n,
                   dict(c for c in wk.items() if c[0] != (waker, how, path)) or '-'))
    # Y spins with SwitchToThread: yields, no blocking wait; P: nothing at all, on the CPU the whole window
    mine, wk = stat('Y')
    y = per[tid['Y']]
    report('Y', y['wait'] < 0.01 * win_ms and y['yield'] > 0.9 * win_ms,
           'blocked %.1f ms, yield runs cover %.1f ms (%d yields), cpu %.1f ms' % (y['wait'], y['yield'], y['yields'], cpu.get(tid['Y'], -1)))
    p = per[tid['P']]
    pe = [ev for ev in events if ev[3] == tid['P']]
    report('P', not pe and cpu.get(tid['P'], 0) > 0.9 * win_ms,
           '%d events, wait %.1f ms, cpu %.1f ms of %.0f' % (len(pe), p['wait'], cpu.get(tid['P'], -1), win_ms))
    # B: Sleep(period) each round
    b = per[tid['B']]
    report('B', b['path:sleep'] > 0.8 * win_ms,
           'sleep %.1f ms, set_event calls %d (%.2f ms in all)' % (b['path:sleep'], b['signals'], b['signal']))
    print('  %s' % ('ALL PASS' if ok else 'SOME FAILED'))
    return 0 if ok else 1


if __name__ == '__main__':
    sys.exit(main())
