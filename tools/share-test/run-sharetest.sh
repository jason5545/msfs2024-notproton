#!/bin/bash
# run-sharetest.sh <Proton dir> <tag> <mode> [VAR=value ...] — r11 video: runs sharetest.exe through the same chain
# Steam uses for MSFS 2024 on steamac (in-place FEX-Emu fex-compat-tool -> SteamLinuxRuntime_sniper -> <Proton dir>)
# with a throwaway compat data dir per <tag> (never compatdata/2537590). mode 0/1/2: see sharetest.c.
# Extra VAR=value are exported (e.g. WINEVULKAN_NO_LINEAR_SHARED_IMAGES=1, WINEDEBUG=...).
set -u
PROTON=$(realpath "$1"); TAG=$2; MODE=$3; shift 3
D=$HOME/steamac-diag/r11v/share
COMMON=$HOME/.local/share/Steam/steamapps/common
FEXDIR="$COMMON/FEX-Emu"
SNIPER="$COMMON/SteamLinuxRuntime_sniper"
OUT=$D/$TAG/mode$MODE
mkdir -p "$OUT" "$D/compat-$TAG"
cp -f "$(dirname "$0")/sharetest.exe" "$D/sharetest.exe" 2>/dev/null || true
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
export WINEDEBUG=+timestamp,+pid,+tid,+loaddll
for kv in "$@"; do export "$kv"; done
unset LD_PRELOAD
cd "$D"
rm -f "$D/sharetest-out-$MODE.txt"
echo "proton $PROTON"; sha256sum "$PROTON/files/lib/wine/x86_64-unix/winevulkan.so"
date '+start %T'
timeout "${TMO:-600}" "$FEXDIR/fex-compat-tool" waitforexitandrun -- "$SNIPER/_v2-entry-point" --verb=waitforexitandrun -- \
  "$PROTON/proton" waitforexitandrun "$D/sharetest.exe" "$MODE" >"$OUT/stdout.txt" 2>"$OUT/stderr.txt"
echo "exit=$?"
date '+end %T'
mv -f "$D/sharetest-out-$MODE.txt" "$OUT/" 2>/dev/null
cat "$OUT/sharetest-out-$MODE.txt" 2>/dev/null || tail -5 "$OUT/stdout.txt"
L=$(ls -t "$OUT"/steam-*.log 2>/dev/null | head -1)
[ -n "$L" ] && grep -E "shared|Shared|LINEAR|OpenSharedHandle|D3DKMT|DDI" "$L" | cut -c1-200 | sort | uniq -c | sort -rn | head -20
