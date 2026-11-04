#!/bin/bash
# jitdump.sh <runtime> <out> <exe> [SECS]: runs a test exe (WARM_MS long) with FEX_BLOCKJITNAMING, samples it SECS
# seconds, then lldb-attaches the TEST PROGRAM (never the game) and dumps every JIT block (code.bin, code.idx).
# Then: jitimg.py <out> (image.bin, pcs.txt), llvm-objcopy/llvm-objdump to image.dis, jitcat.py / jitann.py.
set -u
S=$(cd "$(dirname "$0")" && pwd)
RT=$1; o=$2; EXE=$3; SECS=${4:-6}
export FEX_BLOCKJITNAMING=1 WARM_MS=${WARM_MS:-40000} HOLD=0
for v in FEX_X87REDUCEDPRECISION=1 HB_FEX_SMC_RETIRE=1 HB_FEX_SMC_SKIP_SAME=1 HB_FEX_SMC_HOT_MAXINST=64 \
    HB_FEX_VM_FASTPATH=1 HB_FEX_SMC_DEFER_CALLRET=1; do
  n=${v%%=*}; [ -n "${!n+x}" ] || export "$v"
done
rm -rf "$o"; mkdir -p "$o"
before=$(ls /tmp/perf-*.map 2>/dev/null)
"$S/../hadron/run.sh" "$RT" "${PFX:-$HOME/GitHub/msfs-src/hadron-test/pfx-wasm}" "$o" "$EXE" &
bg=$!
map=
for _ in $(seq 100); do
  sleep 0.3
  for m in /tmp/perf-*.map; do [ -f "$m" ] && ! grep -qx "$m" <<< "$before" && map=$m; done
  [ -n "$map" ] && break
done
pid=${map#/tmp/perf-}; pid=${pid%.map}
sleep 4
sample "$pid" "$SECS" -mayDie -file "$o/sample.txt" > /dev/null 2>&1
cp "$map" "$o/perf.map"
lldb -p "$pid" --batch -o "command script import $S/lldb_dump.py" -o "jitdump $o/perf.map $o" -o "process detach" > "$o/lldb.txt" 2>&1
tail -3 "$o/lldb.txt"
kill $bg 2>/dev/null; pkill -f "$(basename "$EXE")"; sleep 1
wait $bg
rm -f "$map"
