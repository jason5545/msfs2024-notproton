#!/bin/bash
# Runs drtest.exe through the same chain Steam uses for MSFS 2024 on steamac:
#   <FEX-Emu>/fex-compat-tool -> SteamLinuxRuntime_sniper -> Proton 10.0 -> drtest.exe
# with its own throwaway compat data dir (never compatdata/2537590).
# usage: run-drtest.sh <FEX-Emu dir> <tag>
set -u
FEXDIR=$(realpath "$1")
TAG=$2
R5=$HOME/steamac-diag/r5
COMMON=$HOME/.local/share/Steam/steamapps/common
PROTON="$COMMON/Proton 10.0"
SNIPER="$COMMON/SteamLinuxRuntime_sniper"
OUT=$R5/$TAG
mkdir -p "$OUT" "$R5/compat-$TAG"

export STEAM_COMPAT_DATA_PATH=$R5/compat-$TAG
export STEAM_COMPAT_CLIENT_INSTALL_PATH=$HOME/.local/share/Steam
export STEAM_COMPAT_TOOL_PATHS="$PROTON:$SNIPER:$FEXDIR"
export STEAM_COMPAT_MOUNTS="$PROTON:$SNIPER:$FEXDIR:$R5"
export STEAM_COMPAT_INSTALL_PATH=$R5
export STEAM_COMPAT_LIBRARY_PATHS=$HOME/.local/share/Steam/steamapps
export STEAM_COMPAT_APP_ID=0 SteamAppId=0 SteamGameId=0
export STEAM_COMPAT_PROTON=1
export DISPLAY=:1 XDG_RUNTIME_DIR=/run/user/1000
export PROTON_LOG=1 PROTON_LOG_DIR=$OUT
export WINEDEBUG=+timestamp,+pid,+tid,+seh,+server
export STEAMACDR_LOG=1
unset LD_PRELOAD

cd "$R5"
rm -f "$R5/drtest-out.txt"
date '+start %T'
timeout 600 "$FEXDIR/fex-compat-tool" waitforexitandrun -- "$SNIPER/_v2-entry-point" --verb=waitforexitandrun -- \
  "$PROTON/proton" waitforexitandrun "$R5/drtest.exe" >"$OUT/stdout.txt" 2>"$OUT/stderr.txt"
echo "exit=$?"
date '+end %T'
mv -f "$R5/drtest-out.txt" "$OUT/drtest-out.txt" 2>/dev/null
cat "$OUT/stdout.txt" "$OUT/drtest-out.txt" 2>/dev/null
