#!/bin/bash
# fexexp-build.sh: incremental build of the arm64ec FEX DLL in the Hadron fork's FEX tree (scratch/fex/FEX-2610-highball).
cd ~/GitHub/squark-app/scratch/fex/FEX-2610-highball
export PATH=~/GitHub/squark-app/scratch/downloads/llvm-mingw-20261006-ucrt-macos-universal/bin:$PATH
cmake --build build_arm64ec --target arm64ecfex 2>&1 | grep -E 'error|warning: unused|FAILED' | head -20
ls -la build_arm64ec/Bin/libarm64ecfex.dll
