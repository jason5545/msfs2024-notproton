#!/bin/sh
# Builds faulttest.exe (x86-64 PE) with llvm-mingw. Run it with tools/hadron/run.sh under a runtime.
set -eu
cd "$(dirname "$0")"
MINGW=${MINGW:-$HOME/GitHub/squark-app/scratch/downloads/llvm-mingw-20261006-ucrt-macos-universal}
mkdir -p build
"$MINGW/bin/x86_64-w64-mingw32-clang" -O1 -o build/faulttest.exe faulttest.c
shasum -a 256 build/faulttest.exe
