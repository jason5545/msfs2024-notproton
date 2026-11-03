#!/bin/sh
# Builds FEX's ARM64EC unix library for macOS (libarm64ecfex.so, an arm64 Mach-O dylib that Wine loads by name) from
# the FEX fork in $MSFS_SRC/fex-wt (branch steamac-darwin). Its job on macOS is hardware TSO: SetHardwareTSOControl
# calls thread_set_x86_64_compat() (macOS 26 SDK; Wine's loader has the entitlement it needs), and the FEX DLL calls it
# on every thread. Without this library FEX emulates x86 memory ordering in software (see tools/tso-bench).
set -eu
MSFS_SRC=${MSFS_SRC:-$HOME/GitHub/msfs-src}
SRC=$MSFS_SRC/fex-wt/Source/Windows/UnixLib/FEXUnixLib.cpp
OUT=${OUT:-$MSFS_SRC/fex-unixlib-build}
mkdir -p "$OUT"
xcrun clang++ -std=c++20 -O2 -arch arm64 -mmacosx-version-min=26.0 -shared -fPIC -Wall \
  -o "$OUT/libarm64ecfex.so" "$SRC"
nm -u "$OUT/libarm64ecfex.so" | grep -q _thread_set_x86_64_compat || { echo "no thread_set_x86_64_compat import" >&2; exit 1; }
shasum -a 256 "$OUT/libarm64ecfex.so"
