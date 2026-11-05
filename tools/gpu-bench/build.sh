#!/bin/sh
# Builds d3d12gpu.exe (Windows x86-64 PE) with Homebrew's mingw-w64, like tools/d3d-caps.
set -eu
cd "$(dirname "$0")"
mkdir -p build
x86_64-w64-mingw32-gcc -O2 -Wall -Wno-missing-braces -o build/d3d12gpu.exe d3d12gpu.c -ld3d12 -ldxgi -ldxguid
shasum -a 256 build/d3d12gpu.exe
