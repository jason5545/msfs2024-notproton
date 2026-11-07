#!/bin/sh
# rsswatch.sh <runtime> <prefix> <out> <interval s> -- <command…>: runs the command (one that starts a Wine
# program in <prefix> under <runtime>, e.g. the harness's run.sh or tools/hadron/run.sh) and samples the RSS of
# that runtime's wineserver every <interval> seconds into <out>/rss.tsv (seconds, KB), records 25.72.
set -u
RT=$(cd "$1" && pwd); PFX=$2; OUT=$3; IV=$4; shift 5
mkdir -p "$OUT"; : > "$OUT/rss.tsv"
"$@" > "$OUT/cmd.txt" 2>&1 &
cmd=$!
t0=$(date +%s)
while kill -0 $cmd 2>/dev/null; do
  pid=$(pgrep -f "^$RT/.*bin/wineserver" | head -1)
  [ -n "$pid" ] && echo "$(( $(date +%s) - t0 ))	$(ps -o rss= -p "$pid" | tr -d ' ')" >> "$OUT/rss.tsv"
  sleep "$IV"
done
wait $cmd; echo "exit=$?" >> "$OUT/cmd.txt"
