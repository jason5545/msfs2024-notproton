#!/bin/bash
# run-smctest.sh <FEX-Emu dir> <tag> [VAR=value ...] — r10: runs smctest.exe through the same chain Steam uses for
# MSFS 2024 on steamac (fex-compat-tool -> SteamLinuxRuntime_sniper -> Proton 10.0) with a throwaway compat data dir
# (never compatdata/2537590). Extra VAR=value are exported (e.g. STEAMAC_SMC_HOT=0, STEAMACDR_SMCSTATS=1).
set -u
FEXDIR=$(realpath "$1"); TAG=$2; shift 2
D=$HOME/steamac-diag/r10/smc
COMMON=$HOME/.local/share/Steam/steamapps/common
PROTON="$COMMON/Proton 10.0"
SNIPER="$COMMON/SteamLinuxRuntime_sniper"
OUT=$D/$TAG
mkdir -p "$OUT" "$D/compat-$TAG"
export STEAM_COMPAT_DATA_PATH=$D/compat-$TAG
export STEAM_COMPAT_CLIENT_INSTALL_PATH=$HOME/.local/share/Steam
export STEAM_COMPAT_TOOL_PATHS="$PROTON:$SNIPER:$FEXDIR"
export STEAM_COMPAT_MOUNTS="$PROTON:$SNIPER:$FEXDIR:$D"
export STEAM_COMPAT_INSTALL_PATH=$D
export STEAM_COMPAT_LIBRARY_PATHS=$HOME/.local/share/Steam/steamapps
export STEAM_COMPAT_APP_ID=0 SteamAppId=0 SteamGameId=0
export STEAM_COMPAT_PROTON=1
export DISPLAY=:1 XDG_RUNTIME_DIR=/run/user/1000
export PROTON_LOG=1 PROTON_LOG_DIR=$OUT
export WINEDEBUG=-all
for kv in "$@"; do export "$kv"; done
unset LD_PRELOAD
cd "$D"
rm -f "$D/smctest-out.txt"
date '+start %T'
timeout 900 "$FEXDIR/fex-compat-tool" waitforexitandrun -- "$SNIPER/_v2-entry-point" --verb=waitforexitandrun -- \
  "$PROTON/proton" waitforexitandrun "$D/smctest.exe" >"$OUT/stdout.txt" 2>"$OUT/stderr.txt"
echo "exit=$?"
date '+end %T'
mv -f "$D/smctest-out.txt" "$OUT/smctest-out.txt" 2>/dev/null
cat "$OUT/smctest-out.txt" 2>/dev/null
