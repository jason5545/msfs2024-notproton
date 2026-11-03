#!/bin/bash
# Runs one Windows test program under a Hadron runtime (runtime/build-wine.sh's output) in a throwaway
# prefix of its own, never a game's.
#
#   tools/hadron/run.sh <runtime dir> <prefix dir> <out dir> <exe> [args...]
#
# Like compat_run.sh: the loader is lib/wine/<arch>-unix/wine (Wine.app on arm64), WINEDLLPATH the PE and
# unix dirs. HADRON_DLL_OVERLAY, VK_DRIVER_FILES, GST_*, FEX_*, HB_*, VKD3D_*, DXVK_* pass through from the
# environment. Files next to the exe with the same stem (imagequery-helper.dll) are copied too.
# TMO (seconds, default 300) bounds the run. Raw output: <out>/run.txt, stdout.txt, stderr.txt.
set -u
RT=$(cd "$1" && pwd); PFX=$2; OUT=$3; EXE=$4; shift 4
wine_unix="$RT/lib/wine/x86_64-unix"; wine_pe64="$RT/lib/wine/x86_64-windows"
if [ -d "$RT/lib/wine/aarch64-unix" ]; then
  wine_unix="$RT/lib/wine/aarch64-unix"; wine_pe64="$RT/lib/wine/aarch64-windows"
fi
export WINELOADER="$wine_unix/wine" WINESERVER="$RT/bin/wineserver"
export WINEDLLPATH="$wine_pe64:$wine_unix" PATH="$RT/bin:$PATH"
export WINEPREFIX="$PFX" WINEDEBUG=${WINEDEBUG:--all} WINEDLLOVERRIDES=${WINEDLLOVERRIDES:-"mscoree,mshtml="}
mkdir -p "$OUT"; OUT=$(cd "$OUT" && pwd)
cp "$EXE" "$OUT/"
stem=$(basename "$EXE" .exe)
for f in "$(dirname "$EXE")/$stem"-*.dll; do [ -f "$f" ] && cp "$f" "$OUT/"; done
cd "$OUT"
{ echo "runtime=$RT ($(cat "$RT/hadron-runtime" 2>/dev/null))"; echo "prefix=$PFX"; echo "exe=$(basename "$EXE") $*"
  echo "WINEDEBUG=$WINEDEBUG"
  env | grep -E '^(FEX_|HB_|HADRON_|VK_|VKD3D_|DXVK_|GST_|MESA_)' | sort
  for f in "$wine_pe64/xtajit64.dll" "$wine_pe64/xtajit.dll" "$wine_unix/libarm64ecfex.so"; do
    [ -f "$f" ] && shasum -a 256 "$f" | sed "s|$RT/||"
  done
  date '+start %T'; } > run.txt
[ -f "$WINEPREFIX/system.reg" ] || { "$WINELOADER" wineboot --init > boot.txt 2>&1; "$WINESERVER" -w 2>/dev/null; }
start=$(perl -MTime::HiRes=time -e 'printf "%.3f", time')
timeout "${TMO:-300}" "$WINELOADER" "$OUT/$(basename "$EXE")" "$@" > stdout.txt 2> stderr.txt
rc=$?
end=$(perl -MTime::HiRes=time -e 'printf "%.3f", time')
echo "exit=$rc wall=$(echo "$end - $start" | bc)" >> run.txt
date '+end %T' >> run.txt
"$WINESERVER" -k 2>/dev/null; "$WINESERVER" -w 2>/dev/null
exit 0
