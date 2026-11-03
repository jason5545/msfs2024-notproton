#!/bin/sh
# ecdll.dll: ARM64EC, llvm-mingw, static libc++ (like the DXVK/vkd3d-proton ARM64EC builds); ecmain.exe: x86-64.
set -eu
cd "$(dirname "$0")"
LM=${LLVM_MINGW:-$HOME/GitHub/msfs-src/dl/llvm-mingw/llvm-mingw-20261020-ucrt-macos-universal}
mkdir -p build
"$LM/bin/arm64ec-w64-mingw32-clang++" -O2 -shared -static-libstdc++ -static -o build/ecdll.dll ecdll.cpp
"$LM/bin/x86_64-w64-mingw32-clang" -O1 -o build/ecmain.exe ecmain.c
shasum -a 256 build/*.dll build/*.exe
