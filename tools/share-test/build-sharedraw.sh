#!/bin/sh
# Builds sharedraw.exe (x86-64 PE) on the Mac with brew's mingw-w64. d3dcompiler_47 is loaded at run time.
set -eu
cd "$(dirname "$0")"
mkdir -p build
x86_64-w64-mingw32-gcc -O1 -Wall -Wno-missing-braces -o build/sharedraw.exe sharedraw.c -ld3d11 -ld3d12 -ldxgi -ldxguid
shasum -a 256 build/sharedraw.exe
