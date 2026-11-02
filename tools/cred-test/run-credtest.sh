#!/bin/bash
# Runs credtest.exe under a NotProton runner in its own throwaway prefix (never the game's compatdata).
# usage: run-credtest.sh <runner CrossOver dir> <prefix dir> <out dir> [args]
# WINEDEBUG defaults to -all; set it in the environment to override.
set -u
CX_ROOT=$1; PFX=$2; OUT=$3; shift 3
HERE=$(cd "$(dirname "$0")" && pwd)
export CX_ROOT CX_HOME="$HOME/Library/Application Support/CrossOver"
wine_unix="$CX_ROOT/lib/wine/aarch64-unix"
export WINELOADER="$wine_unix/wine.app/Contents/MacOS/wine" WINESERVER="$CX_ROOT/bin/wineserver-arm64"
export WINEDLLPATH="$CX_ROOT/lib/wine/x86_64-windows:$wine_unix" PATH="$CX_ROOT/bin:$PATH"
export WINEPREFIX="$PFX" WINEDEBUG=${WINEDEBUG:--all} WINEDLLOVERRIDES="mscoree,mshtml="
mkdir -p "$OUT"
cp "$HERE/build/credtest.exe" "$OUT/"
cd "$OUT"
{ echo "runner=$CX_ROOT"; echo "prefix=$PFX"; echo "WINEDEBUG=$WINEDEBUG"; echo "args=$*"; date '+start %T'; } > run.txt
[ -f "$WINEPREFIX/system.reg" ] || "$WINELOADER" wineboot --init > boot.txt 2>&1
date '+booted %T' >> run.txt
timeout ${TMO:-120} "$WINELOADER" "$OUT/credtest.exe" "$@" > stdout.txt 2> stderr.txt
echo "exit=$?" >> run.txt
"$WINESERVER" -w 2>/dev/null
date '+done %T' >> run.txt
cat stdout.txt
