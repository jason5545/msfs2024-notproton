#!/bin/sh
# A/B of KK 0015 (command pool keeps the BOs its command buffers use, records 25.72) with d3d12gpu cpool:
# a = runtime without it, b = with it, c = with it and MESA_KK_CMD_POOL_FIXED_CAP=1 (old cap), rounds
# alternating a b c, each shape "lists:allocs", fresh output folders under <out>.
#   tools/gpu-bench/cpool-ab.sh <runtime a> <runtime b> <prefix (absolute)> <out> [rounds] [frames] [shapes…]
set -eu
A=$1; B=$2; PFX=$3; OUT=$4; ROUNDS=${5:-3}; FRAMES=${6:-300}; shift 6 2>/dev/null || shift $#
SHAPES=${*:-"8:1 32:1 64:1 128:1 32:4 8:16"}
here=$(cd "$(dirname "$0")" && pwd)
mkdir -p "$OUT"
for r in $(seq 1 "$ROUNDS"); do
  for sh in $SHAPES; do
    L=${sh%%:*}; N=${sh##*:}
    for v in a b c; do
      case $v in a) rt=$A; fx= ;; b) rt=$B; fx= ;; c) rt=$B; fx=1 ;; esac
      o=$OUT/r$r-$v-L$L-A$N
      if [ -n "$fx" ]; then
        MESA_KK_CMD_POOL_FIXED_CAP=1 "$here/run.sh" "$rt" "$PFX" "$o" cpool "$L" "$N" "$FRAMES" 8 > /dev/null 2>&1 || true
      else
        "$here/run.sh" "$rt" "$PFX" "$o" cpool "$L" "$N" "$FRAMES" 8 > /dev/null 2>&1 || true
      fi
      printf '%s %s L=%s A=%s ' "r$r" "$v" "$L" "$N"
      grep -h -e 'median ms' -e 'whole run' -e 'timestamps' "$o/stdout.txt" | tr '\n' ' '
      echo
    done
  done
done
