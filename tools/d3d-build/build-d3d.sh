#!/bin/bash
# build-d3d.sh <dxvk|vkd3d> <name> — cross-builds the DXVK or vkd3d-proton fork ($FORKS/*-steamac, default ~/GitHub; whatever is checked
# out there) with meson, release, stripped, the way their package-release.sh does.
#   ARCH=x64 (default)  Windows x86-64 with brew's mingw-w64 (gcc). In the game these run inside FEX with the game.
#   ARCH=arm64ec        ARM64EC with llvm-mingw ($LLVM_MINGW, default ~/GitHub/msfs-src/dl/llvm-mingw/...20261020...)
#                       and cross-arm64ec.txt next to this script. An x64 process loads them like x64 DLLs, but their
#                       code runs natively instead of in FEX (the PE header says AMD64, the code is ARM64EC).
# Build tree: ~/GitHub/msfs-src/d3d-build/<name>/build; DLLs: ~/GitHub/msfs-src/d3d-build/<name>/<ARCH>.
# Runs under nice with JOBS (default 6) jobs. Reconfigures only if the build tree is missing.
set -euo pipefail
WHAT=$1; NAME=$2
FORKS=${FORKS:-$HOME/GitHub}
OUT=$HOME/GitHub/msfs-src/d3d-build/$NAME
ARCH=${ARCH:-x64}
case $ARCH in
  x64)     CROSS=build-win64.txt; CC_VER=x86_64-w64-mingw32-gcc ;;
  arm64ec) CROSS=$(cd "$(dirname "$0")" && pwd)/cross-arm64ec.txt; CC_VER=arm64ec-w64-mingw32-clang
           export PATH="${LLVM_MINGW:-$HOME/GitHub/msfs-src/dl/llvm-mingw/llvm-mingw-20261020-ucrt-macos-universal}/bin:$PATH" ;;
  *) echo "ARCH must be x64 or arm64ec" >&2; exit 2 ;;
esac
case $WHAT in
  dxvk)  SRC=$FORKS/dxvk-steamac;       OPTS=(-Db_ndebug=if-release -Denable_d3d8=false -Denable_d3d9=false) ;;
  vkd3d) SRC=$FORKS/vkd3d-proton-steamac; OPTS=(-Denable_tests=false -Denable_extras=false) ;;
  *) echo "usage: $0 <dxvk|vkd3d> <name>" >&2; exit 2 ;;
esac
mkdir -p "$OUT"
if [ ! -f "$OUT/build/build.ninja" ]; then
  (cd "$SRC" && meson setup --cross-file "$CROSS" --buildtype release --strip \
     --prefix "$OUT" --bindir "$ARCH" --libdir "$ARCH" "${OPTS[@]}" "$OUT/build") > "$OUT/setup.log" 2>&1 || { tail -30 "$OUT/setup.log"; exit 1; }
fi
cd "$OUT/build"
nice -n 10 ninja -j "${JOBS:-6}" install > "$OUT/ninja.log" 2>&1 || { grep -E 'error|FAILED' -A3 "$OUT/ninja.log" | head -60; exit 1; }
{ echo "src=$SRC arch=$ARCH"; echo "commit=$(git -C "$SRC" rev-parse HEAD) $(git -C "$SRC" status --porcelain --untracked-files=no | wc -l | tr -d ' ') modified"; date '+built %F %T'
  $CC_VER --version | head -1; meson --version; ls "$OUT/$ARCH"/*.dll | xargs shasum -a 256; } > "$OUT/build-info.txt"
cat "$OUT/build-info.txt"
