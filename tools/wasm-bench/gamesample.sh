#!/bin/bash
# gamesample.sh <outdir> [seconds]: sample the running MSFS like game-sample-1104-1956 (sample, vmmap, modules, gpu)
set -u
O=${1:?outdir}; S=${2:-10}
mkdir -p "$O"
P=$(pgrep -f 'MSFS2024\\FlightSimulator2024\.exe' | head -1)
[ -n "$P" ] || { echo "game not running"; exit 1; }
echo "pid $P $(date '+%F %T')" > "$O/info.txt"
( for i in $(seq 1 "$S"); do
    ioreg -r -d 1 -c IOAccelerator 2>/dev/null | grep -o '"Renderer Utilization %"=[0-9]*\|"Device Utilization %"=[0-9]*\|"In use system memory"=[0-9]*' | tr '\n' ' '; echo
    sleep 1; done > "$O/gpu.txt" ) &
G=$!
sample "$P" "$S" -file "$O/sample.txt" >/dev/null 2>&1
wait $G
vmmap -wide "$P" > "$O/vmmap.txt" 2>/dev/null
awk '{ for (i = 1; i <= NF; i++) if ($i ~ /^[0-9a-f]+-[0-9a-f]+$/) { r = $i; break }
       p = index($0, " /"); if (!r || !p) next; path = substr($0, p + 1)
       if (path in seen) next; seen[path] = 1; split(r, a, "-"); print a[1], a[2], path; r = "" }' "$O/vmmap.txt" > "$O/modules.txt"
ps -o %cpu=,rss= -p "$P" >> "$O/info.txt"
python3 "$HOME/GitHub/squark/tools/wasm-bench/gameprof.py" "$O" > "$O/gameprof.txt" 2>&1
echo "$O"; head -12 "$O/gameprof.txt"
