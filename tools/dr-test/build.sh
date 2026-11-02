#!/bin/sh
# Builds drtest.exe (x86-64 PE, console, no CRT) with Homebrew LLVM on the Mac.
set -eu
cd "$(dirname "$0")"
LLVM=/opt/homebrew/opt/llvm/bin
OUT=build
mkdir -p "$OUT"
cat > "$OUT/kernel32.def" <<'EOF'
LIBRARY kernel32.dll
EXPORTS
AddVectoredExceptionHandler
GetCurrentThread
GetCurrentThreadId
SetThreadContext
GetThreadContext
GetLastError
GetStdHandle
WriteFile
ExitProcess
CreateThread
WaitForSingleObject
SuspendThread
ResumeThread
Sleep
CreateFileA
CloseHandle
EOF
"$LLVM/llvm-dlltool" -m i386:x86-64 -d "$OUT/kernel32.def" -l "$OUT/kernel32.lib"
"$LLVM/clang" -target x86_64-pc-windows-msvc -O1 -ffreestanding -fno-builtin -fno-stack-protector -mno-stack-arg-probe \
  -c drtest.c -o "$OUT/drtest.obj"
/opt/homebrew/bin/lld-link /nologo /entry:Entry /subsystem:console /nodefaultlib /out:"$OUT/drtest.exe" "$OUT/drtest.obj" "$OUT/kernel32.lib"
shasum -a 256 "$OUT/drtest.exe"
