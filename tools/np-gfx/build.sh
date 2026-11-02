#!/bin/sh
# Builds vktest.exe and d3d12test.exe (Windows x64) with llvm-mingw.
#   LLVM_MINGW=<llvm-mingw dir> tools/np-gfx/build.sh
# Vulkan headers come from Homebrew's vulkan-headers.
set -eu
cd "$(dirname "$0")"
: "${LLVM_MINGW:?set LLVM_MINGW to the llvm-mingw folder}"
CC="$LLVM_MINGW/bin/x86_64-w64-mingw32-clang"
mkdir -p build
"$CC" -O1 -Wall -I/opt/homebrew/include -o build/vktest.exe vktest.c
"$CC" -O1 -Wall -o build/d3d12test.exe d3d12test.c -ld3d12 -ldxgi -luuid
shasum -a 256 build/vktest.exe build/d3d12test.exe
