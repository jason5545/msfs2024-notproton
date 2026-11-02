#!/bin/sh
# Builds credtest.exe (x86-64 PE, no CRT) with Homebrew LLVM, like tools/eh-test.
set -eu
cd "$(dirname "$0")"
LLVM=/opt/homebrew/opt/llvm/bin
OUT=build
mkdir -p "$OUT"
printf 'LIBRARY kernel32.dll\nEXPORTS\nGetStdHandle\nWriteFile\nExitProcess\nGetLastError\nSetLastError\nGetCommandLineA\n' > "$OUT/kernel32.def"
printf 'LIBRARY advapi32.dll\nEXPORTS\nCredWriteW\nCredReadW\nCredEnumerateW\nCredDeleteW\nCredFree\nGetUserNameW\n' > "$OUT/advapi32.def"
printf 'LIBRARY bcrypt.dll\nEXPORTS\nBCryptOpenAlgorithmProvider\nBCryptSetProperty\nBCryptGenerateKeyPair\nBCryptFinalizeKeyPair\nBCryptExportKey\nBCryptImportKeyPair\nBCryptSignHash\nBCryptVerifySignature\nBCryptDestroyKey\n' > "$OUT/bcrypt.def"
for d in kernel32 advapi32 bcrypt; do "$LLVM/llvm-dlltool" -m i386:x86-64 -d "$OUT/$d.def" -l "$OUT/$d.lib"; done
"$LLVM/clang" -target x86_64-pc-windows-msvc -O1 -ffreestanding -fno-builtin -fno-stack-protector -mno-stack-arg-probe \
  -c credtest.c -o "$OUT/credtest.obj"
/opt/homebrew/bin/lld-link /nologo /entry:Entry /subsystem:console /nodefaultlib \
  /out:"$OUT/credtest.exe" "$OUT/credtest.obj" "$OUT/kernel32.lib" "$OUT/advapi32.lib" "$OUT/bcrypt.lib"
shasum -a 256 "$OUT/credtest.exe"
