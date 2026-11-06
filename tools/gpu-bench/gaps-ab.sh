#!/bin/bash
# A/B of d3d12gpu gaps between two runtimes (records 25.59): arm a = runtime A, b = runtime B, c = runtime B
# with C_ENV (default MESA_KK_EAGER_TS_RESOLVE=1, KosmicKrisp 0014's switch back). Alternates the arms, ROUNDS
# (3) rounds per variant.
#   tools/gpu-bench/gaps-ab.sh <runtime A> <runtime B> <prefix (absolute)> <out dir>
# VARIANTS (default "gfx tsgfx tsdraw tsresolve tsrescs tslists"), ARMS ("a b c"), N (64), FRAMES (500),
# ITERS (100), GROUPS (4). Writes <out>/<variant>-<arm>-r<n>/ (run.sh output) and <out>/summary.tsv; summarize
# with gaps_sum.py.
set -u
RTA=$1; RTB=$2; PFX=$3; OUT=$4
here=$(cd "$(dirname "$0")" && pwd)
VARIANTS=${VARIANTS:-"gfx tsgfx tsdraw tsresolve tsrescs tslists"}; ARMS=${ARMS:-"a b c"}; ROUNDS=${ROUNDS:-3}
N=${N:-64}; FRAMES=${FRAMES:-500}; ITERS=${ITERS:-100}; GROUPS=${GROUPS:-4}
C_ENV=${C_ENV:-MESA_KK_EAGER_TS_RESOLVE=1}
mkdir -p "$OUT"
printf 'variant\tarm\tround\tframe_us\tspan_med_us\tspan_min_us\tchecksum\n' > "$OUT/summary.tsv"
for r in $(seq 1 "$ROUNDS"); do for v in $VARIANTS; do for a in $ARMS; do
  d="$OUT/$v-$a-r$r"; rm -rf "$d"
  case $a in a) rt=$RTA; e=;; b) rt=$RTB; e=;; c) rt=$RTB; e=$C_ENV;; esac
  env $e TMO=120 "$here/run.sh" "$rt" "$PFX" "$d" gaps "$v" "$N" "$FRAMES" "$ITERS" "$GROUPS" > /dev/null 2>&1
  s="$d/stdout.clean.txt"; tr -d '\r' < "$d/stdout.txt" > "$s"
  pd=$(sed -n 's/^per dispatch: frame \([0-9.]*\) us  GPU span median \([0-9.]*\) us  min \([0-9.]*\) us/\1\t\2\t\3/p' "$s")
  ck=$(sed -n 's/^checksum //p' "$s")
  printf '%s\t%s\t%s\t%s\t%s\n' "$v" "$a" "$r" "${pd:-FAIL}" "${ck:-}" >> "$OUT/summary.tsv"
done; done; done
column -t -s $'\t' "$OUT/summary.tsv"
