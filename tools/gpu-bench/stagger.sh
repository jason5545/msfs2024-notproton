#!/bin/bash
# A/B of vkd3d-proton's staggered submissions (REC=fresh re-records the lists every frame) (records 25.58): d3d12gpu stagger with and without
# VKD3D_CONFIG=no_staggered_submit, alternating, ROUNDS (3) rounds per point, over LISTS x ITERS.
#   tools/gpu-bench/stagger.sh <runtime> <prefix (absolute)> <out dir>
# Writes <out>/<point>-<a|b>-r<n>/ (run.sh output) and <out>/summary.tsv.
set -u
RT=$1; PFX=$2; OUT=$3
here=$(cd "$(dirname "$0")" && pwd)
LISTS=${LISTS:-"8 32 96"}; ITERS=${ITERS:-"500 2000 8000"}; ROUNDS=${ROUNDS:-3}; FRAMES=${FRAMES:-300}
MODES=${MODES:-"two one"}; REC=${REC:-reuse}
mkdir -p "$OUT"
printf 'mode\tlists\titers\tcfg\tround\tframe_ms\tinterval_med\tinterval_p90\tgpu_work\tbusy\tchecksum\tstaggered\n' > "$OUT/summary.tsv"
for mode in $MODES; do for l in $LISTS; do for i in $ITERS; do for r in $(seq 1 "$ROUNDS"); do for c in a b; do
  d="$OUT/$mode-$REC-l$l-i$i-$c-r$r"; rm -rf "$d"
  if [ $c = b ]; then cfg=no_staggered_submit; else cfg=; fi
  VKD3D_CONFIG=$cfg VKD3D_DEBUG=info TMO=120 "$here/run.sh" "$RT" "$PFX" "$d" stagger "$FRAMES" "$l" "$i" "$mode" "$REC" > /dev/null 2>&1
  s="$d/stdout.clean.txt"; tr -d '\r' < "$d/stdout.txt" > "$s"
  fm=$(sed -n 's/^frame ms (total\/frames) \([0-9.]*\).*interval median \([0-9.]*\)  p90 \([0-9.]*\).*/\1\t\2\t\3/p' "$s")
  gw=$(sed -n 's/^GPU ms per frame: work median \([0-9.]*\).*busy (work \/ frame) \([0-9.]*\)%/\1\t\2/p' "$s")
  ck=$(sed -n 's/^checksum //p' "$s")
  st=$(grep -c 'Enabling staggered' "$d/stderr.txt")
  printf '%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\n' "$mode" "$l" "$i" "${cfg:-default}" "$r" "${fm:-FAIL}" "${gw:-}" "${ck:-}" "$st" >> "$OUT/summary.tsv"
done; done; done; done; done
column -t -s $'\t' "$OUT/summary.tsv"
