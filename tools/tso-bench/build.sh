#!/bin/sh
# Builds tsobench.exe (x86-64 PE, console, no CRT) with Homebrew LLVM on the Mac (same way as tools/dr-test).
set -eu
cd "$(dirname "$0")"
LLVM=/opt/homebrew/opt/llvm/bin
OUT=build
mkdir -p "$OUT"
cat > "$OUT/kernel32.def" <<'DEF'
LIBRARY kernel32.dll
EXPORTS
AddVectoredExceptionHandler
GetCurrentThread
SetThreadContext
GetStdHandle
WriteFile
ExitProcess
CreateThread
WaitForSingleObject
Sleep
CreateFileA
CloseHandle
VirtualAlloc
QueryPerformanceCounter
QueryPerformanceFrequency
DEF
"$LLVM/llvm-dlltool" -m i386:x86-64 -d "$OUT/kernel32.def" -l "$OUT/kernel32.lib"
"$LLVM/clang" -target x86_64-pc-windows-msvc -O1 -ffreestanding -fno-builtin -fno-stack-protector -mno-stack-arg-probe \
  -c tsobench.c -o "$OUT/tsobench.obj"
/opt/homebrew/bin/lld-link /nologo /entry:Entry /subsystem:console /nodefaultlib /out:"$OUT/tsobench.exe" "$OUT/tsobench.obj" "$OUT/kernel32.lib"
shasum -a 256 "$OUT/tsobench.exe"
