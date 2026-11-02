#!/bin/bash
# Runs ehtest.exe under a NotProton runner with its own throwaway prefix (never the game's compatdata).
# usage: run-ehtest.sh <runner CrossOver dir> <prefix dir> <out dir> [vcruntime140.dll to load as native]
# WINEDEBUG defaults to -all; set it in the environment to override.
set -u
CX_ROOT=$1; PFX=$2; OUT=$3; VCRT=${4:-}
HERE=$(cd "$(dirname "$0")" && pwd)
export CX_ROOT CX_HOME="$HOME/Library/Application Support/CrossOver"
wine_unix="$CX_ROOT/lib/wine/aarch64-unix"
export WINELOADER="$wine_unix/wine.app/Contents/MacOS/wine" WINESERVER="$CX_ROOT/bin/wineserver-arm64"
export WINEDLLPATH="$CX_ROOT/lib/wine/x86_64-windows:$wine_unix" PATH="$CX_ROOT/bin:$PATH"
export WINEPREFIX="$PFX" WINEDEBUG=${WINEDEBUG:--all}
OV="mscoree,mshtml="
mkdir -p "$OUT"
cp "$HERE/build/ehtest.exe" "$OUT/"
if [ -n "$VCRT" ]; then cp "$VCRT" "$OUT/vcruntime140.dll"; OV="$OV;vcruntime140=n"; fi
export WINEDLLOVERRIDES="$OV"
cd "$OUT"
{ echo "runner=$CX_ROOT"; echo "prefix=$PFX"; echo "vcruntime140=${VCRT:-builtin}"; echo "WINEDEBUG=$WINEDEBUG"; echo "ARGS=${ARGS:-}";
  echo "WINEDLLOVERRIDES=$WINEDLLOVERRIDES"; date '+start %T'; } > run.txt
[ -f "$WINEPREFIX/system.reg" ] || "$WINELOADER" wineboot --init > boot.txt 2>&1
date '+booted %T' >> run.txt
timeout ${TMO:-180} "$WINELOADER" "$OUT/ehtest.exe" ${ARGS:-} > stdout.txt 2> stderr.txt
echo "exit=$?" >> run.txt
date '+end %T' >> run.txt
"$WINESERVER" -k 2>/dev/null; "$WINESERVER" -w 2>/dev/null
date '+done %T' >> run.txt
cat stdout.txt
