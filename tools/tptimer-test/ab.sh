#!/bin/bash
# ab.sh <out> <prefix> <runtime>...: runs tptimer.exe once per mode under each runtime, with the wait recorder
# (Wine patch 0061) on the timer queue thread, and the wineserver's CPU time over the same window.
#
#   MODES  space-separated mode[,ms[,timers]] (default "none idle abs rel per,16 absper,16 rel,20 abs,20")
#   SECS   tptimer.exe run length (default 6); the recording window is WL_SECONDS (default 4) starting 1 s after
#          READY (warmup 0.5 s)
#   ROUNDS how many times to go over the runtimes (default 1); runtimes alternate within a round
#
# Output per run: <out>/<runtime name>/<round>-<mode>/ (stdout.txt, run.txt, waitlog/, focus.txt, wineserver.txt),
# and one line per run in <out>/summary.txt.
set -u
T=$(cd "$(dirname "$0")/../.." && pwd)
OUT=$1; PFX=$2; shift 2
EXE=$T/tools/tptimer-test/build/tptimer.exe
MODES=${MODES:-"none idle abs rel per,16 absper,16 rel,20 abs,20"}
SECS=${SECS:-6}; WLS=${WL_SECONDS:-4}; ROUNDS=${ROUNDS:-1}
mkdir -p "$OUT"; OUT=$(cd "$OUT" && pwd)

cputime() { # seconds of CPU of a pid ("ps -o time" is [[hh:]mm:]ss.cc)
  ps -o time= -p "$1" 2>/dev/null | awk -F: '{ s = 0; for (i = 1; i <= NF; i++) s = s * 60 + $i; printf "%.2f", s }'
}

for r in $(seq 1 "$ROUNDS"); do
  for m in $MODES; do
    IFS=, read -r mode ms nt <<< "$m"
    for rt in "$@"; do
      RT=$(cd "$rt" && pwd); name=$(basename "$RT")
      dir=$OUT/$name/$r-${m//,/-}
      rm -rf "$dir"; mkdir -p "$dir"
      args=("$mode" "$SECS")
      [ -n "$ms" ] && args+=("$ms")
      [ -n "$nt" ] && args+=("$nt")
      WL_SECONDS=$WLS WL_AT=1 "$T/tools/waitlog-test/run.sh" "$RT" "$PFX" "$dir" "$EXE" "${args[@]}" \
        > "$dir/runsh.txt" 2>&1 &
      bg=$!
      until grep -q '^READY' "$dir/stdout.txt" 2>/dev/null; do kill -0 $bg 2>/dev/null || break; sleep 0.05; done
      sleep 1.5
      ws=$(pgrep -f "^$RT/(lib/wine/\.\./\.\./)?bin/wineserver" | head -1)
      c0=$(cputime "$ws"); t0=$(perl -MTime::HiRes=time -e 'printf "%.3f", time')
      sleep "$WLS"
      c1=$(cputime "$ws"); t1=$(perl -MTime::HiRes=time -e 'printf "%.3f", time')
      wait $bg
      echo "wineserver pid=$ws cpu_s=$c0..$c1 wall_s=$t0..$t1" > "$dir/wineserver.txt"
      ws_pct=$(echo "$c0 $c1 $t0 $t1" | awk '{ if ($4 > $3) printf "%.1f", ($2 - $1) * 100 / ($4 - $3); else print "?" }')
      json=$(ls "$dir"/waitlog/*.json 2>/dev/null | head -1)
      tq=""; line="no recording"
      if [ -n "$json" ]; then
        tq=$(python3 -c 'import json,sys
d=json.load(open(sys.argv[1]))
print(next((str(t["tid"]) for t in d["threads"] if t.get("name")=="wine_threadpool_timerqueue"), ""))' "$json")
        if [ -n "$tq" ]; then
          python3 "$T/tools/waitlog/waitlog.py" "$json" --focus "$tq" --top 6 --threads 6 > "$dir/focus.txt" 2>&1
          line=$(grep -E "^focus thread $tq " "$dir/focus.txt" | head -1)
        else
          python3 "$T/tools/waitlog/waitlog.py" "$json" --top 6 --threads 6 > "$dir/focus.txt" 2>&1
          line="no timer queue thread"
        fi
      fi
      cb=$(grep -E '^mode=' "$dir/stdout.txt" | sed 's/.*callbacks=//')
      pc=$(grep -E '^process cpu_ms' "$dir/stdout.txt" | sed 's/.*total=//')
      iv=$(grep -E '^interval_ms' "$dir/stdout.txt" | sed 's/^interval_ms //')
      lt=$(grep -E '^first callback' "$dir/stdout.txt" | sed 's/^first callback //')
      printf '%s r%s %-12s | timerqueue: %s | wineserver %s%% | callbacks %s | process %s | %s %s\n' \
        "$name" "$r" "$m" "$line" "$ws_pct" "$cb" "$pc" "$lt" "$iv" | tee -a "$OUT/summary.txt"
    done
  done
done
