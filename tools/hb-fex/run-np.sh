#!/bin/bash
# Runs one Windows x64 test program under a NotProton (CrossOver 27 Preview) runner copy with its own throwaway prefix,
# never the game's.
# usage: run-np.sh <runner CrossOver dir> <prefix dir> <out dir> <exe> [args...]
# Companion files next to the exe with the same stem (imagequery-helper.dll) are copied too. Environment (FEX_*,
# STEAMAC_*, HB_FEX_*) passes through to the program. TMO (seconds, default 300) bounds the run.
set -u
CX_ROOT=$1; PFX=$2; OUT=$3; EXE=$4; shift 4
export CX_ROOT CX_HOME="$HOME/Library/Application Support/CrossOver"
wine_unix="$CX_ROOT/lib/wine/aarch64-unix"
export WINELOADER="$wine_unix/wine.app/Contents/MacOS/wine" WINESERVER="$CX_ROOT/bin/wineserver-arm64"
export WINEDLLPATH="$CX_ROOT/lib/wine/x86_64-windows:$wine_unix" PATH="$CX_ROOT/bin:$PATH"
export WINEPREFIX="$PFX" WINEDEBUG=${WINEDEBUG:--all} WINEDLLOVERRIDES="mscoree,mshtml="
mkdir -p "$OUT"; OUT=$(cd "$OUT" && pwd)
cp "$EXE" "$OUT/"
stem=$(basename "$EXE" .exe)
for f in "$(dirname "$EXE")/$stem"-*.dll; do [ -f "$f" ] && cp "$f" "$OUT/"; done
cd "$OUT"
{ echo "runner=$CX_ROOT"; echo "prefix=$PFX"; echo "exe=$(basename "$EXE") $*"; echo "WINEDEBUG=$WINEDEBUG";
  env | grep -E '^(FEX_|STEAMAC_|HB_FEX_)' | sort;
  shasum -a 256 "$CX_ROOT/lib/wine/aarch64-windows/xtajit64.dll" | sed 's/^/runner fex /';
  [ -f "$PFX/drive_c/windows/system32/xtajit64.dll" ] && shasum -a 256 "$PFX/drive_c/windows/system32/xtajit64.dll" | sed 's/^/prefix fex /';
  [ -f "$wine_unix/libarm64ecfex.so" ] && shasum -a 256 "$wine_unix/libarm64ecfex.so" | sed 's/^/unixlib /';
  date '+start %T'; } > run.txt
[ -f "$WINEPREFIX/system.reg" ] || "$WINELOADER" wineboot --init > boot.txt 2>&1
start=$(perl -MTime::HiRes=time -e 'printf "%.3f", time')
timeout ${TMO:-300} "$WINELOADER" "$OUT/$(basename "$EXE")" "$@" > stdout.txt 2> stderr.txt
rc=$?
end=$(perl -MTime::HiRes=time -e 'printf "%.3f", time')
echo "exit=$rc wall=$(echo "$end - $start" | bc)" >> run.txt
date '+end %T' >> run.txt
"$WINESERVER" -k 2>/dev/null; "$WINESERVER" -w 2>/dev/null
exit 0
