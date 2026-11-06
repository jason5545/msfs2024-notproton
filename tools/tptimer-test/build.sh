#!/bin/sh
# Builds tptimer.exe (x64, runs on FEX under Wine) with llvm-mingw from the Squark app's scratch dir.
set -eu
D=$(cd "$(dirname "$0")" && pwd)
LLVM_MINGW=${LLVM_MINGW:-$HOME/GitHub/squark-app/scratch/downloads/llvm-mingw-20261006-ucrt-macos-universal}
mkdir -p "$D/build"
"$LLVM_MINGW/bin/x86_64-w64-mingw32-clang" -O2 -Wall -o "$D/build/tptimer.exe" "$D/tptimer.c"
echo "$D/build/tptimer.exe"
