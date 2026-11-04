#!/bin/bash
# swap.sh <hb dir> <xtajit64.dll> [libarm64ecfex.so]: puts the FEX DLL into the test runner copy (<hb dir>/CrossOver) and
# its own prefix (<hb dir>/pfx), and the unix library into the runner when given. Never the installed runner, never the
# game's prefix.
set -eu
H=$1
cp "$2" "$H/CrossOver/lib/wine/aarch64-windows/xtajit64.dll"
[ -d "$H/pfx/drive_c/windows/system32" ] && cp "$2" "$H/pfx/drive_c/windows/system32/xtajit64.dll"
[ $# -ge 3 ] && cp "$3" "$H/CrossOver/lib/wine/aarch64-unix/libarm64ecfex.so"
shasum -a 256 "$H/CrossOver/lib/wine/aarch64-windows/xtajit64.dll" "$H/CrossOver/lib/wine/aarch64-unix/libarm64ecfex.so" | cut -c1-16
