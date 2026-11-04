#!/bin/sh
# Builds the code-footprint benchmark for NF generated functions: gen-big.py -> wasm32 -> wasm2c -> x64 PE
# (build/big-<NF>/bigbench.exe, -march=x86-64-v2) and the arm64 macOS reference (build/big-<NF>/bigbench-mac).
# Prints the size of the x64 .text, the code FEX has to translate.
#
#   build-big.sh <NF>...
set -eu
cd "$(dirname "$0")"
LLVM=/opt/homebrew/opt/llvm/bin
WABT=$(brew --prefix wabt)
RT=$WABT/share/wabt/wasm2c
[ -f "$RT/wasm-rt-impl.c" ] || RT=$(dirname "$(find "$WABT/" -name wasm-rt-impl.c | head -1)")
MINGW=${MINGW:-$HOME/GitHub/msfs-src/forks/Hadron-steamac/scratch/downloads/llvm-mingw-20261006-ucrt-macos-universal}
for nf in "$@"; do
  O=build/big-$nf
  mkdir -p "$O"
  python3 gen-big.py "$nf" > "$O/big-src.c"
  "$LLVM/clang" --target=wasm32 -O2 -nostdlib -Wl,--no-entry -Wl,--export-dynamic -o "$O/big.wasm" "$O/big-src.c"
  wasm2c --module-name=big "$O/big.wasm" -o "$O/big.c"
  SRC="host-big.c $O/big.c $RT/wasm-rt-impl.c $RT/wasm-rt-mem-impl.c"
  INC="-I$O -I$WABT/include -I$RT"
  "$MINGW/bin/x86_64-w64-mingw32-clang" -O2 -march=x86-64-v2 $INC $SRC -o "$O/bigbench.exe" -static &
  "$LLVM/clang" -O2 -mcpu=apple-m1 $INC $SRC -o "$O/bigbench-mac" &
  wait
  text=$("$LLVM/llvm-objdump" -h "$O/bigbench.exe" | awk '$2==".text"{print strtonum("0x"$3)}' 2>/dev/null || true)
  [ -n "$text" ] || text=$("$LLVM/llvm-size" -A "$O/bigbench.exe" | awk '$1==".text"{print $2}')
  echo "big-$nf x64_text_bytes=$text"
done
