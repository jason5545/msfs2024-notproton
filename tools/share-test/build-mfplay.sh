#!/bin/sh
# Builds mfplay.exe (x86-64 PE) on the Mac with brew's mingw-w64.
set -eu
cd "$(dirname "$0")"
mkdir -p build
x86_64-w64-mingw32-gcc -O1 -Wall -Wno-missing-braces -o build/mfplay.exe mfplay.c -lmfplat -lmfuuid -lole32 -loleaut32 -ld3d11 -ld3d12 -ldxgi -ldxguid
shasum -a 256 build/mfplay.exe
