#!/bin/sh
# Builds smcmsfs.exe (x86-64 PE, console) with llvm-mingw.
set -eu
cd "$(dirname "$0")"
BIN=${LLVM_MINGW:-$HOME/GitHub/msfs-src/dl/llvm-mingw/llvm-mingw-20261020-ucrt-macos-universal/bin}
mkdir -p build
"$BIN/x86_64-w64-mingw32-clang" -O2 -Wall -o build/smcmsfs.exe smcmsfs.c
shasum -a 256 build/smcmsfs.exe
