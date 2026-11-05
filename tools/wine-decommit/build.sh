#!/bin/sh
# Builds decommit.exe (x86-64 PE, runs under FEX) and decommit-arm64.exe (native arm64 PE) with llvm-mingw. Run them
# with tools/hadron/run.sh under a runtime.
set -eu
cd "$(dirname "$0")"
MINGW=${MINGW:-$HOME/GitHub/squark-app/scratch/downloads/llvm-mingw-20261006-ucrt-macos-universal}
mkdir -p build
"$MINGW/bin/x86_64-w64-mingw32-clang" -O1 -o build/decommit.exe decommit.c
"$MINGW/bin/aarch64-w64-mingw32-clang" -O1 -o build/decommit-arm64.exe decommit.c
shasum -a 256 build/decommit.exe build/decommit-arm64.exe
