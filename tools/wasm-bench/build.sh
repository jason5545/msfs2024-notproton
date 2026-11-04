#!/bin/sh
# Builds the WebAssembly avionics benchmark the way an AOT WASM compiler (MSFS compiles gauges to a
# native DLL with LLVM) would: avionics.c -> wasm32 -> wasm2c -> C -> native code.
#
#   build/wasmbench-sse.exe    x64, -march=x86-64-v2 (SSE4.2, no AVX)
#   build/wasmbench-avx2.exe   x64, -march=skylake: what LLVM's host-CPU detection picks from the CPUID
#                              FEX reports (GenuineIntel family 6 model 0xA6, AVX2/FMA/BMI2 on)
#   build/wasmbench-mac        arm64 macOS, the reference without translation
#
# Needs Homebrew llvm, lld (wasm-ld), wabt (wasm2c) and llvm-mingw (MINGW, default the one in the Hadron
# fork's scratch/downloads).
set -eu
cd "$(dirname "$0")"
LLVM=/opt/homebrew/opt/llvm/bin
WABT=$(brew --prefix wabt)
RT=$WABT/share/wabt/wasm2c
[ -f "$RT/wasm-rt-impl.c" ] || RT=$(dirname "$(find "$WABT/" -name wasm-rt-impl.c | head -1)")
MINGW=${MINGW:-$HOME/GitHub/squark-app/scratch/downloads/llvm-mingw-20261006-ucrt-macos-universal}
OUT=build
mkdir -p "$OUT"

"$LLVM/clang" --target=wasm32 -O2 -nostdlib -Wl,--no-entry -Wl,--export-dynamic \
  -o "$OUT/avionics.wasm" avionics.c
wasm2c --module-name=avionics "$OUT/avionics.wasm" -o "$OUT/avionics.c"

SRC="host.c $OUT/avionics.c $RT/wasm-rt-impl.c $RT/wasm-rt-mem-impl.c"
INC="-I$OUT -I$WABT/include -I$RT"
for v in sse:x86-64-v2 avx2:skylake; do
  name=${v%%:*}; march=${v#*:}
  "$MINGW/bin/x86_64-w64-mingw32-clang" -O2 -march="$march" $INC $SRC -o "$OUT/wasmbench-$name.exe" -static
done
# memory.copy without leaving x64: memmove in the exe itself (memmove-local.c)
for v in rep:MM_REP sse16:MM_SSE; do
  "$MINGW/bin/x86_64-w64-mingw32-clang" -O2 -march=x86-64-v2 -Dmemmove=bench_memmove -D${v#*:} $INC $SRC \
    memmove-local.c -o "$OUT/wasmbench-mm${v%%:*}.exe" -static
done
"$LLVM/clang" -O2 -mcpu=apple-m1 $INC $SRC -o "$OUT/wasmbench-mac"
shasum -a 256 "$OUT"/wasmbench-*
