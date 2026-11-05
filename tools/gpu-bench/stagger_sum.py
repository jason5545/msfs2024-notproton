#!/usr/bin/env python3
"""Summary of tools/gpu-bench/stagger.sh's summary.tsv: per point, median (min) frame ms of A (default) and
B (VKD3D_CONFIG=no_staggered_submit), GPU busy, B/A, whether every checksum of the point matches, and how
many queues logged 'Enabling staggered' (records 25.58)."""
import csv, sys, statistics as st
from collections import defaultdict
g = defaultdict(list)
for r in csv.DictReader(open(sys.argv[1]), delimiter='\t'):
    g[(r['mode'], int(r['lists']), int(r['iters']), r['cfg'])].append(r)
print('mode lists iters | A frame ms med (min)  busy | B frame ms med (min)  busy |  B/A | checksum | staggered A,B | GPU work A, B')
for k in sorted({k[:3] for k in g}):
    a, b = g[k + ('default',)], g[k + ('no_staggered_submit',)]
    fa = [float(x['frame_ms']) for x in a]; fb = [float(x['frame_ms']) for x in b]
    med = lambda xs, f: st.median(float(x[f]) for x in xs)
    same = 'same' if len({x['checksum'] for x in a + b}) == 1 else 'DIFF'
    print(f"{k[0]} {k[1]:3d} {k[2]:5d} | {st.median(fa):8.3f} ({min(fa):8.3f}) {med(a,'busy'):5.1f}% |"
          f" {st.median(fb):8.3f} ({min(fb):8.3f}) {med(b,'busy'):5.1f}% | {st.median(fb)/st.median(fa):.3f} | {same} |"
          f" {','.join(sorted({x['staggered'] for x in a}))} / {','.join(sorted({x['staggered'] for x in b}))} |"
          f" {med(a,'gpu_work'):.3f}, {med(b,'gpu_work'):.3f}")
