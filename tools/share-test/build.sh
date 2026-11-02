#!/bin/sh
# Builds sharetest.exe (x86-64 PE, mingw-w64 CRT) on agent-hub (Ubuntu 24.04, gcc-mingw-w64-x86-64).
# Run from this directory on a machine with x86_64-w64-mingw32-gcc.
set -eu
cd "$(dirname "$0")"
mkdir -p build
x86_64-w64-mingw32-gcc -O1 -Wall -Wno-missing-braces -o build/sharetest.exe sharetest.c -ld3d11 -ld3d12 -ldxgi -ldxguid
sha256sum build/sharetest.exe
