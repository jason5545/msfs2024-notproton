#!/bin/bash
# Runs tsobench.exe under a NotProton (CrossOver 27 Preview) runner copy with its own throwaway prefix, never the game's.
# usage: run-np.sh <runner CrossOver dir> <prefix dir> <out dir>
# FEX settings come from the environment (e.g. FEX_TSOENABLED=0); they are passed through to the program.
set -u
CX_ROOT=$1; PFX=$2; OUT=$3
HERE=$(cd "$(dirname "$0")" && pwd)
export CX_ROOT CX_HOME="$HOME/Library/Application Support/CrossOver"
wine_unix="$CX_ROOT/lib/wine/aarch64-unix"
export WINELOADER="$wine_unix/wine.app/Contents/MacOS/wine" WINESERVER="$CX_ROOT/bin/wineserver-arm64"
export WINEDLLPATH="$CX_ROOT/lib/wine/x86_64-windows:$wine_unix" PATH="$CX_ROOT/bin:$PATH"
export WINEPREFIX="$PFX" WINEDEBUG=${WINEDEBUG:--all} WINEDLLOVERRIDES="mscoree,mshtml="
mkdir -p "$OUT"
cp "$HERE/build/tsobench.exe" "$OUT/"
cd "$OUT"
{ echo "runner=$CX_ROOT"; echo "prefix=$PFX"; env | grep '^FEX_' ;
  shasum -a 256 "$CX_ROOT/lib/wine/aarch64-windows/xtajit64.dll" | sed 's/^/runner fex /';
  [ -f "$PFX/drive_c/windows/system32/xtajit64.dll" ] && shasum -a 256 "$PFX/drive_c/windows/system32/xtajit64.dll" | sed 's/^/prefix fex /';
  ls "$CX_ROOT/lib/wine/aarch64-unix/" | grep -i arm64ecfex | sed 's/^/unixlib /'; date '+start %T'; } > run.txt
[ -f "$WINEPREFIX/system.reg" ] || "$WINELOADER" wineboot --init > boot.txt 2>&1
timeout ${TMO:-300} "$WINELOADER" "$OUT/tsobench.exe" > stdout.txt 2> stderr.txt
echo "exit=$?" >> run.txt
"$WINESERVER" -k 2>/dev/null; "$WINESERVER" -w 2>/dev/null
date '+end %T' >> run.txt
cat tsobench-out.txt
