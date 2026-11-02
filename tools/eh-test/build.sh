#!/bin/sh
# Builds ehtest.exe (x86-64 PE, MSVC C++ ABI, no CRT) with Homebrew LLVM on the Mac.
# C++ exceptions go through the game's own vcruntime140.dll (_CxxThrowException, __CxxFrameHandler3).
set -eu
cd "$(dirname "$0")"
LLVM=/opt/homebrew/opt/llvm/bin
OUT=build
mkdir -p "$OUT"
cat > "$OUT/kernel32.def" <<'EOF'
LIBRARY kernel32.dll
EXPORTS
GetStdHandle
WriteFile
ExitProcess
GetCurrentThreadId
ConvertThreadToFiber
CreateFiber
SwitchToFiber
CreateThread
WaitForSingleObject
RaiseException
CreateFileA
GetCommandLineA
AddVectoredExceptionHandler
GetCurrentThread
SetThreadContext
EOF
cat > "$OUT/vcruntime140.def" <<'EOF'
LIBRARY vcruntime140.dll
EXPORTS
_CxxThrowException
__CxxFrameHandler3
__C_specific_handler
__std_terminate
memcpy
memset
EOF
cat > "$OUT/ucrtbase.def" <<'EOF'
LIBRARY ucrtbase.dll
EXPORTS
qsort
EOF
for d in kernel32 vcruntime140 ucrtbase; do
  "$LLVM/llvm-dlltool" -m i386:x86-64 -d "$OUT/$d.def" -l "$OUT/$d.lib"
done
"$LLVM/clang++" -target x86_64-pc-windows-msvc -O1 -std=c++17 -fms-extensions -fexceptions -fcxx-exceptions \
  -ffreestanding -fno-builtin -fno-stack-protector -mno-stack-arg-probe -c ehtest.cpp -o "$OUT/ehtest.obj"
/opt/homebrew/bin/lld-link /nologo /entry:Entry /subsystem:console /nodefaultlib /stack:0x180000,0x1000 \
  /out:"$OUT/ehtest.exe" "$OUT/ehtest.obj" "$OUT/kernel32.lib" "$OUT/vcruntime140.lib" "$OUT/ucrtbase.lib"
shasum -a 256 "$OUT/ehtest.exe"
