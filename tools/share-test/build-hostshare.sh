#!/bin/sh
# Builds hostshare.exe (x86-64 PE) on the Mac with brew's mingw-w64; Vulkan headers from brew's vulkan-headers.
# -idirafter keeps mingw's own headers ahead of anything else in /opt/homebrew/include.
set -eu
cd "$(dirname "$0")"
mkdir -p build
x86_64-w64-mingw32-gcc -O1 -Wall -Wno-missing-braces -idirafter /opt/homebrew/include -o build/hostshare.exe hostshare.c
shasum -a 256 build/hostshare.exe
