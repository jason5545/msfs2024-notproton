#!/bin/bash
# Runs smctest.exe under a NotProton (CrossOver 27 Preview) runner with its own throwaway prefix, never the game's.
# usage: run-smctest-np.sh <runner CrossOver dir> <prefix dir> <out dir>
# The FEX the runner loads is <runner>/lib/wine/aarch64-windows/xtajit64.dll, and the prefix keeps its own copy in
# drive_c/windows/system32 (wineboot copies it once); swap both. WINEDEBUG defaults to -all. Extra environment
# (STEAMAC_SMC_HOT, STEAMAC_SMCSTATS) is passed through to the program.
set -u
CX_ROOT=$1; PFX=$2; OUT=$3
HERE=$(cd "$(dirname "$0")" && pwd)
export CX_ROOT CX_HOME="$HOME/Library/Application Support/CrossOver"
wine_unix="$CX_ROOT/lib/wine/aarch64-unix"
export WINELOADER="$wine_unix/wine.app/Contents/MacOS/wine" WINESERVER="$CX_ROOT/bin/wineserver-arm64"
export WINEDLLPATH="$CX_ROOT/lib/wine/x86_64-windows:$wine_unix" PATH="$CX_ROOT/bin:$PATH"
export WINEPREFIX="$PFX" WINEDEBUG=${WINEDEBUG:--all} WINEDLLOVERRIDES="mscoree,mshtml="
mkdir -p "$OUT"
cp "$HERE/build/smctest.exe" "$OUT/"
cd "$OUT"
{ echo "runner=$CX_ROOT"; echo "prefix=$PFX"; echo "WINEDEBUG=$WINEDEBUG";
  echo "STEAMAC_SMC_HOT=${STEAMAC_SMC_HOT-unset} STEAMAC_SMCSTATS=${STEAMAC_SMCSTATS-unset}";
  shasum -a 256 "$CX_ROOT/lib/wine/aarch64-windows/xtajit64.dll" | sed 's/^/runner fex /';
  [ -f "$PFX/drive_c/windows/system32/xtajit64.dll" ] && shasum -a 256 "$PFX/drive_c/windows/system32/xtajit64.dll" | sed 's/^/prefix fex /';
  date '+start %T'; } > run.txt
[ -f "$WINEPREFIX/system.reg" ] || "$WINELOADER" wineboot --init > boot.txt 2>&1
date '+booted %T' >> run.txt
timeout ${TMO:-180} "$WINELOADER" "$OUT/smctest.exe" > stdout.txt 2> stderr.txt
echo "exit=$?" >> run.txt
date '+end %T' >> run.txt
"$WINESERVER" -k 2>/dev/null; "$WINESERVER" -w 2>/dev/null
date '+done %T' >> run.txt
cat smctest-out.txt
