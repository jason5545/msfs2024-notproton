#!/bin/bash
# Builds an ARM64X msvproc.dll (Wine's video processor MFT, swscale from the bundled FFmpeg 8.1.1) from upstream
# Wine 11.17 + patches/wine/msvproc-arm64ec-yuv2rgb.patch (ARM64EC yuv2rgb_neon.S overwrote B of pixels 0-7 of every
# 16 for BGRA output: the striped MSFS loading screen, logs/notproton-log.md section 16).
# usage: build.sh [msfs-src dir]   -> <msfs-src>/wine-vproc-build/out/msvproc.dll
#        ORIG=1 build.sh           -> same tree without the patch (control), out/msvproc-orig.dll
set -eu
SRC=${1:-$HOME/GitHub/msfs-src}
PATCH=$(cd "$(dirname "$0")/../.." && pwd)/patches/wine/msvproc-arm64ec-yuv2rgb.patch
LM=$SRC/dl/llvm-mingw/llvm-mingw-20261020-ucrt-macos-universal/bin
export PATH=/opt/homebrew/opt/bison/bin:/opt/homebrew/opt/flex/bin:$LM:/opt/homebrew/bin:/usr/bin:/bin:/usr/sbin:/sbin
WT=$SRC/wine-vproc
[ -d "$WT" ] || git -C "$SRC/wine-11.17" worktree add --detach "$WT" wine-11.17
git -C "$WT" checkout -q --detach wine-11.17 2>/dev/null || git -C "$WT" checkout -q --detach 36b6a2c
[ "${ORIG:-0}" = 1 ] || git -C "$WT" apply "$PATCH"
B=$SRC/wine-vproc-build; mkdir -p "$B"; cd "$B"
[ -f config.status ] || ../wine-vproc/configure --enable-archs=arm64ec,aarch64 --disable-tests \
  --without-x --without-freetype --without-gnutls --without-gstreamer --without-sdl --without-vulkan \
  --without-opencl --without-pcap --without-cups --without-sane --without-krb5 --without-netapi \
  --without-gphoto --without-usb --without-v4l2 --without-wayland --without-dbus --without-pulse \
  --without-alsa --without-oss --without-udev --without-capi --without-coreaudio --without-pcsclite \
  --without-inotify --without-gettext --without-ffmpeg --without-opengl --without-xinerama > configure.out 2>&1
make -j12 dlls/msvproc/aarch64-windows/msvproc.dll > make.out 2>&1
mkdir -p out
N=msvproc.dll; [ "${ORIG:-0}" = 1 ] && N=msvproc-orig.dll
cp dlls/msvproc/aarch64-windows/msvproc.dll out/$N && "$LM/llvm-strip" out/$N
git -C "$WT" checkout -q -- libs/ffmpeg
shasum -a 256 out/$N
