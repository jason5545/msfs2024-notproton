#!/bin/sh
# Builds d3d12caps.exe (Windows x86-64 PE) with brew's mingw-w64. Run with tools/d3d-caps/run.sh.
set -eu
cd "$(dirname "$0")"
mkdir -p build
x86_64-w64-mingw32-gcc -O2 -Wall -Wno-missing-braces -o build/d3d12caps.exe d3d12caps.c -ld3d12 -ldxgi -ldxguid
shasum -a 256 build/d3d12caps.exe
