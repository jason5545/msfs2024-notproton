#!/usr/bin/env python3
"""Summary of tools/gpu-bench/gaps-ab.sh's summary.tsv (records 25.59): per variant and arm, the median over the
rounds of the per-dispatch GPU span median, the smallest per-dispatch minimum and the per-dispatch frame time
(microseconds), b/a, and whether every checksum of the variant matches."""
import csv, sys, statistics as st
from collections import defaultdict
g = defaultdict(list)
for r in csv.DictReader(open(sys.argv[1]), delimiter='\t'):
    g[(r['variant'], r['arm'])].append(r)
variants = list(dict.fromkeys(r[0] for r in g))
arms = sorted({r[1] for r in g})
print('variant    ' + ' | '.join(f'{a}: span med (min)  frame' for a in arms) + ' | b/a span | checksum')
for v in variants:
    cells, med = [], {}
    for a in arms:
        xs = g.get((v, a), [])
        if not xs or any(x['span_med_us'] in ('', 'FAIL') for x in xs):
            cells.append(f'{a}: FAIL'); continue
        med[a] = st.median(float(x['span_med_us']) for x in xs)
        cells.append(f"{a}: {med[a]:7.2f} ({min(float(x['span_min_us']) for x in xs):6.2f}) "
                     f"{st.median(float(x['frame_us']) for x in xs):7.2f}")
    sums = {x['checksum'] for a in arms for x in g.get((v, a), [])}
    ratio = f"{med['b'] / med['a']:.3f}" if 'a' in med and 'b' in med else '-'
    print(f"{v:10s} " + ' | '.join(cells) + f" | {ratio:>8s} | {'same' if len(sums) == 1 else 'DIFF ' + ' '.join(sorted(sums))}")
