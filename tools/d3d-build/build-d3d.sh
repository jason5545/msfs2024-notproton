#!/bin/bash
# build-d3d.sh <dxvk|vkd3d> <name> — cross-builds the DXVK or vkd3d-proton fork (forks/*-steamac, whatever is checked
# out there) for Windows x86-64 with brew's mingw-w64 + meson, release, stripped, the way their package-release.sh
# does. Build tree: ~/GitHub/msfs-src/d3d-build/<name>/build; DLLs: ~/GitHub/msfs-src/d3d-build/<name>/x64.
# Runs under nice with JOBS (default 6) jobs. Reconfigures only if the build tree is missing.
set -euo pipefail
WHAT=$1; NAME=$2
R=$(cd "$(dirname "$0")/../.." && pwd)
OUT=$HOME/GitHub/msfs-src/d3d-build/$NAME
case $WHAT in
  dxvk)  SRC=$R/forks/dxvk-steamac;         OPTS=(-Db_ndebug=if-release -Denable_d3d8=false -Denable_d3d9=false) ;;
  vkd3d) SRC=$R/forks/vkd3d-proton-steamac; OPTS=(-Denable_tests=false -Denable_extras=false) ;;
  *) echo "usage: $0 <dxvk|vkd3d> <name>" >&2; exit 2 ;;
esac
mkdir -p "$OUT"
if [ ! -f "$OUT/build/build.ninja" ]; then
  (cd "$SRC" && meson setup --cross-file "$SRC/build-win64.txt" --buildtype release --strip \
     --prefix "$OUT" --bindir x64 --libdir x64 "${OPTS[@]}" "$OUT/build") > "$OUT/setup.log" 2>&1 || { tail -30 "$OUT/setup.log"; exit 1; }
fi
cd "$OUT/build"
nice -n 10 ninja -j "${JOBS:-6}" install > "$OUT/ninja.log" 2>&1 || { grep -E 'error|FAILED' -A3 "$OUT/ninja.log" | head -60; exit 1; }
{ echo "src=$SRC"; echo "commit=$(git -C "$SRC" rev-parse HEAD) $(git -C "$SRC" status --porcelain --untracked-files=no | wc -l | tr -d ' ') modified"; date '+built %F %T'
  x86_64-w64-mingw32-gcc --version | head -1; meson --version; ls "$OUT/x64"/*.dll | xargs shasum -a 256; } > "$OUT/build-info.txt"
cat "$OUT/build-info.txt"
