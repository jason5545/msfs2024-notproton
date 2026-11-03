#!/bin/sh
# Builds d3dbench.exe (Windows x86-64 PE) with brew's mingw-w64. d3dcompiler_47 is loaded at run time.
set -eu
cd "$(dirname "$0")"
mkdir -p build
x86_64-w64-mingw32-gcc -O2 -Wall -Wno-missing-braces -o build/d3dbench.exe d3dbench.c -ld3d11 -ld3d12 -ldxgi -ldxguid
shasum -a 256 build/d3dbench.exe
