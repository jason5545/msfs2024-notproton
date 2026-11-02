#!/bin/sh
# Builds mfgrab.exe (x86-64 PE, brew mingw-w64) and ppmcmp (native, compares two PPM frames per column mod 16).
set -eu
cd "$(dirname "$0")"
mkdir -p build
x86_64-w64-mingw32-gcc -O1 -Wall -Wno-missing-braces -o build/mfgrab.exe mfgrab.c -lmfplat -lmfreadwrite -lmfuuid -lole32 -loleaut32 -ld3d11 -ld3d12 -ldxgi -ldxguid -lwindowscodecs
cc -O2 -Wall -o build/ppmcmp ppmcmp.c
shasum -a 256 build/mfgrab.exe build/ppmcmp
