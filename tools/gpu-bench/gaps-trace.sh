#!/bin/bash
# Records the GPU of a d3d12gpu gaps run with Instruments (records 25.59): starts the run in the background,
# waits WAIT (2) s, records the GPU instrument for LIMIT (3s) over all processes (it is system wide anyway),
# exports metal-gpu-intervals and notes the program's pid for tools/d3d-caps/mst_encgaps.py.
#   tools/gpu-bench/gaps-trace.sh <runtime> <prefix (absolute)> <out dir> <name> <gaps args...>
# Writes <out>/<name>/ (run.sh output), <name>.trace, <name>-intervals.xml (both large, keep them out of the
# repos) and <name>.pid. Give the run enough frames to outlast WAIT + LIMIT.
set -u
RT=$1; PFX=$2; OUT=$3; NAME=$4; shift 4
here=$(cd "$(dirname "$0")" && pwd)
mkdir -p "$OUT"; cd "$OUT"
TMO=120 "$here/run.sh" "$RT" "$PFX" "$NAME" gaps "$@" > "$NAME.runlog" 2>&1 &
bg=$!
sleep "${WAIT:-2}"
rm -rf "$NAME.trace"
xcrun xctrace record --instrument GPU --all-processes --time-limit "${LIMIT:-3s}" --output "$NAME.trace" \
  > "$NAME.xctrace.txt" 2>&1
echo "xctrace exit $?"
wait $bg
xcrun xctrace export --input "$NAME.trace" \
  --xpath '/trace-toc/run[@number="1"]/data/table[@schema="metal-gpu-intervals"]' > "$NAME-intervals.xml"
pid=$(grep -m1 -o 'd3d12gpu.exe ([0-9]*)' "$NAME-intervals.xml" | head -1 | sed 's/.*(//; s/)//')
echo "$pid" > "$NAME.pid"
echo "pid $pid"
tr -d '\r' < "$NAME/stdout.txt" | grep -E '^gaps|^per dispatch|^checksum'
