#!/bin/bash
# replay.sh <label> [rounds=3]: the A380X PFD (Index=2) in msfs-host.js (25.43's JS replay, a380-bench/jitperf/ru.sh:
# instructions and cycles per frame over 1,500 frames) under: base = runtime without Wine patch 0061 (rt-wait-base),
# off = patched without a squark-waitlog folder, idle = patched with the folder (watcher asleep), stats = off plus
# FEX_PROFILESTATS=1. Same FEX in all (the runtime's xtajit64.dll), MSFS's FEX switches (ru.sh), alternating per round.
set -u
T=$HOME/GitHub/msfs-src/hadron-test; RU=$HOME/GitHub/msfs-src/a380-bench/jitperf/ru.sh
L=$1; N=${2:-3}
DLL=$T/rt-wait/lib/wine/aarch64-windows/xtajit64.dll
cmp -s "$DLL" "$T/rt-wait-base/lib/wine/aarch64-windows/xtajit64.dll" || { echo "FEX differs between the runtimes"; exit 1; }
rm -rf "$T/pfx-wait2/squark-waitlog"; mkdir -p "$T/pfx-wait/squark-waitlog"
for i in $(seq 1 "$N"); do
  RT=$T/rt-wait-base PFX=$T/pfx-wait2 "$RU" "$L-base-$i" 1 A380X/PFD 'Index=2&duID=3' "base=$DLL" | sed "s/^1 /$i /"
  RT=$T/rt-wait PFX=$T/pfx-wait2 "$RU" "$L-off-$i" 1 A380X/PFD 'Index=2&duID=3' "off=$DLL" | sed "s/^1 /$i /"
  RT=$T/rt-wait PFX=$T/pfx-wait "$RU" "$L-idle-$i" 1 A380X/PFD 'Index=2&duID=3' "idle=$DLL" | sed "s/^1 /$i /"
  RT=$T/rt-wait PFX=$T/pfx-wait2 "$RU" "$L-stats-$i" 1 A380X/PFD 'Index=2&duID=3' "stats=$DLL,FEX_PROFILESTATS=1" | sed "s/^1 /$i /"
done
