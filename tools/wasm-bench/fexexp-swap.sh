#!/bin/bash
# fexexp-swap.sh exp|orig : put the FEX build output (exp) or the original xtajit64.dll (d07bdc18) into rt-fexexp AND the
# test prefix pfx-wasm's system32 (Wine loads xtajit64.dll from the prefix's system32, not the runtime's lib dir).
D=~/GitHub/msfs-src/hadron-test/rt-fexexp/lib/wine/aarch64-windows/xtajit64.dll
P=~/GitHub/msfs-src/hadron-test/pfx-wasm/drive_c/windows/system32/xtajit64.dll
WB=~/GitHub/msfs-src/forks/Hadron-steamac/scratch/runtimes/wine-11.18-fex/build/tools/winebuild/winebuild
if [ "$1" = orig ]; then
  cp "$HOME/Library/Application Support/squark/runtimes/wine-11.18-fex/lib/wine/aarch64-windows/xtajit64.dll" "$D"
else
  cp ~/GitHub/msfs-src/forks/Hadron-steamac/scratch/fex/FEX-2610-highball/build_arm64ec/Bin/libarm64ecfex.dll "$D"
  "$WB" --builtin "$D"
fi
cp "$D" "$P"
shasum -a 256 "$D" "$P" | cut -c1-16 | tr '\n' ' '; echo
