#!/bin/bash
# Builds an ARM64X advapi32.dll from upstream Wine 11.17 + patches/wine/advapi32-steamac.patch
# (CredEnumerateW keeps registry results when the macOS keychain has no match; CW Hack 12735 username).
# CW 27 Preview's wine reports wine-11.17-8910-g4683e69e153, so 11.17 is its open-source base.
# usage: build.sh [msfs-src dir]   -> <msfs-src>/wine-adv-build/out/advapi32.dll
set -eu
SRC=${1:-$HOME/GitHub/msfs-src}
PATCH=$(cd "$(dirname "$0")/../.." && pwd)/patches/wine/advapi32-steamac.patch
LM=$SRC/dl/llvm-mingw/llvm-mingw-20261020-ucrt-macos-universal/bin
export PATH=/opt/homebrew/opt/bison/bin:/opt/homebrew/opt/flex/bin:$LM:/opt/homebrew/bin:/usr/bin:/bin:/usr/sbin:/sbin
WT=$SRC/wine-steamac
if [ ! -d "$WT" ]; then
  git -C "$SRC/wine-11.17" worktree add -b steamac-advapi32 "$WT" wine-11.17 2>/dev/null \
    || git -C "$SRC/wine-11.17" worktree add -b steamac-advapi32 "$WT" HEAD
fi
git -C "$WT" checkout -q -- dlls/advapi32
git -C "$WT" apply "$PATCH"
B=$SRC/wine-adv-build; mkdir -p "$B"; cd "$B"
[ -f config.status ] || ../wine-steamac/configure --enable-archs=arm64ec,aarch64 --disable-tests \
  --without-x --without-freetype --without-gnutls --without-gstreamer --without-sdl --without-vulkan \
  --without-opencl --without-pcap --without-cups --without-sane --without-krb5 --without-netapi \
  --without-gphoto --without-usb --without-v4l2 --without-wayland --without-dbus --without-pulse \
  --without-alsa --without-oss --without-udev --without-capi --without-coreaudio --without-pcsclite \
  --without-inotify --without-gettext --without-ffmpeg --without-opengl --without-xinerama > configure.out 2>&1
make -j12 dlls/advapi32/aarch64-windows/advapi32.dll > make.out 2>&1
mkdir -p out && cp dlls/advapi32/aarch64-windows/advapi32.dll out/ && "$LM/llvm-strip" out/advapi32.dll
shasum -a 256 out/advapi32.dll
