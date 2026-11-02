#!/bin/sh
# Builds smctest.exe (x86-64 PE, console, no CRT) with Homebrew LLVM on the Mac (same way as tools/dr-test).
set -eu
cd "$(dirname "$0")"
LLVM=/opt/homebrew/opt/llvm/bin
OUT=build
mkdir -p "$OUT"
cat > "$OUT/kernel32.def" <<'DEF'
LIBRARY kernel32.dll
EXPORTS
VirtualAlloc
QueryPerformanceCounter
QueryPerformanceFrequency
GetStdHandle
WriteFile
ExitProcess
CreateFileA
CloseHandle
CreateThread
WaitForSingleObject
VirtualProtect
DEF
"$LLVM/llvm-dlltool" -m i386:x86-64 -d "$OUT/kernel32.def" -l "$OUT/kernel32.lib"
"$LLVM/clang" -target x86_64-pc-windows-msvc -O1 -ffreestanding -fno-builtin -fno-stack-protector -mno-stack-arg-probe \
  -c smctest.c -o "$OUT/smctest.obj"
/opt/homebrew/bin/lld-link /nologo /entry:Entry /subsystem:console /nodefaultlib /out:"$OUT/smctest.exe" "$OUT/smctest.obj" "$OUT/kernel32.lib"
shasum -a 256 "$OUT/smctest.exe"
