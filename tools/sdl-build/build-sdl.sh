#!/bin/sh
# Builds SDL3 for the runtime's winebus SDL bus: the release Homebrew ships (3.4.20 since 11/7)
# with patches/sdl on top, arm64, as one libSDL3.0.dylib in $MSFS_SRC/sdl/stage.
# tools/hadron/build-runtime.sh hands that file to build-wine.sh (HADRON_SDL3) in place of
# Homebrew's; sdl2-compat stays Homebrew's.
#
#   tools/sdl-build/build-sdl.sh
#
# The source tree is a clone of libsdl-org/SDL at $SDL_TAG; each run puts it back to the tag
# and applies every patch again, so a changed patch needs nothing by hand.
set -eu
here="$(cd "$(dirname "$0")" && pwd)"
repo="$(cd "$here/../.." && pwd)"
MSFS_SRC="${MSFS_SRC:-$HOME/GitHub/msfs-src}"
SDL_TAG="${SDL_TAG:-release-3.4.20}"
SRC="${SDL_SRC:-$MSFS_SRC/sdl/SDL-${SDL_TAG#release-}}"
BUILD="$MSFS_SRC/sdl/build-${SDL_TAG#release-}"
STAGE="$MSFS_SRC/sdl/stage"
MACOS_MIN="${MACOS_MIN:-26.5}"

if [ ! -d "$SRC/.git" ]; then
    git clone -q --depth 1 --branch "$SDL_TAG" https://github.com/libsdl-org/SDL.git "$SRC"
fi
tag_commit="$(git -C "$SRC" rev-parse "$SDL_TAG^{commit}")"
git -C "$SRC" -c advice.detachedHead=false checkout -q --detach "$tag_commit"
git -C "$SRC" reset -q --hard "$tag_commit"
for p in "$repo"/patches/sdl/*.patch; do
    echo "==> $(basename "$p")"
    git -C "$SRC" apply --whitespace=nowarn "$p"
done

cmake -S "$SRC" -B "$BUILD" -G Ninja \
    -DCMAKE_BUILD_TYPE=Release \
    -DCMAKE_OSX_ARCHITECTURES=arm64 \
    -DCMAKE_OSX_DEPLOYMENT_TARGET="$MACOS_MIN" \
    -DSDL_SHARED=ON -DSDL_STATIC=OFF -DSDL_TEST_LIBRARY=OFF -DSDL_TESTS=OFF -DSDL_EXAMPLES=OFF \
    -DSDL_X11=OFF >/dev/null
cmake --build "$BUILD"

mkdir -p "$STAGE"
rm -f "$STAGE/libSDL3.0.dylib"
cp "$(readlink -f "$BUILD/libSDL3.0.dylib")" "$STAGE/libSDL3.0.dylib"
cp "$SRC/LICENSE.txt" "$STAGE/LICENSE.txt"
git -C "$SRC" diff --stat "$tag_commit" | tail -1
echo "staged $STAGE/libSDL3.0.dylib"
