#!/bin/bash
# variant.sh status|install|uninstall — puts the self-built msvproc.dll (build.sh) into NotProton as variant component
# "steamacvproc", the same way steamacadv carries advapi32 (logs/notproton-log.md 15.7, 16).
#
#   status     sha256 of the built DLL, the component copy, CW 27's original and the game prefix's system32 copy;
#              whether variants/steamac.conf has the [steamacvproc] section.
#   install    checks the built DLL against EXPECT, copies it to notproton/steamac/msvproc/aarch64-windows/,
#              backs up variants/steamac.conf to ~/GitHub/msfs-src/backup/ and appends the section if missing.
#              Takes effect only when the launch option lists it: NOTPROTON_RUNNER_VARIANT=...+steamacvproc
#   uninstall  removes the section (backup first) and the component folder. Dropping "+steamacvproc" from the
#              launch option is enough to go back; uninstall is only for cleaning up.
set -eu
EXPECT=${EXPECT:-}   # sha256 the built DLL must have; printed by build.sh and written in log section 16
NP="$HOME/Library/Application Support/notproton"
CONF="$NP/variants/steamac.conf"
COMP="$NP/steamac/msvproc/aarch64-windows"
BUILT="$HOME/GitHub/msfs-src/wine-vproc-build/out/msvproc.dll"
CW_ORIG="$NP/runners/crossover-27.0.0.41073-fex/CrossOver/lib/wine/aarch64-windows/msvproc.dll"
PFX_DLL="$HOME/Library/Application Support/Steam/steamapps/compatdata/2537590/pfx/drive_c/windows/system32/msvproc.dll"
BK="$HOME/GitHub/msfs-src/backup"
sha() { [ -f "$1" ] && shasum -a 256 "$1" | cut -c1-64 || echo "(missing)"; }

case "${1:-status}" in
status)
  echo "built      $(sha "$BUILT")"
  echo "component  $(sha "$COMP/msvproc.dll")"
  echo "CW 27 orig $(sha "$CW_ORIG")"
  echo "prefix     $(sha "$PFX_DLL")"
  grep -q '^\[steamacvproc\]' "$CONF" && echo "conf: [steamacvproc] present" || echo "conf: [steamacvproc] absent"
  ;;
install)
  [ -n "$EXPECT" ] || { echo "set EXPECT=<sha256 of the built msvproc.dll>" >&2; exit 2; }
  [ "$(sha "$BUILT")" = "$EXPECT" ] || { echo "built DLL sha256 $(sha "$BUILT") != EXPECT" >&2; exit 3; }
  mkdir -p "$COMP" "$BK"
  cp "$BUILT" "$COMP/msvproc.dll"
  [ "$(sha "$COMP/msvproc.dll")" = "$EXPECT" ] || { echo "component copy mismatch" >&2; exit 4; }
  if ! grep -q '^\[steamacvproc\]' "$CONF"; then
    cp -p "$CONF" "$BK/variants-steamac.conf.$(date +%m%d-%H%M%S)"
    cat >> "$CONF" <<'EOS'

[steamacvproc]
label   = msvproc（上游 Wine 11.17 原始碼＋ARM64EC yuv2rgb 暫存器修正，patches/wine/msvproc-arm64ec-yuv2rgb.patch）
variant = steamac msvproc
src     = Library/Application Support/notproton/steamac/msvproc
dst     = lib/wine
build   = 27.0.0.41073-fex
EOS
  fi
  echo "installed $EXPECT; launch option: NOTPROTON_RUNNER_VARIANT=steamacfex+steamackk+steamacadv+steamacvproc"
  ;;
uninstall)
  if grep -q '^\[steamacvproc\]' "$CONF"; then
    mkdir -p "$BK"; cp -p "$CONF" "$BK/variants-steamac.conf.$(date +%m%d-%H%M%S)"
    python3 -I - "$CONF" <<'EOP'
import sys, re
p = sys.argv[1]; s = open(p, encoding='utf-8').read()
s = re.sub(r'\n*\[steamacvproc\]\n(?:[^\[\n][^\n]*\n?)*', '\n', s).rstrip('\n') + '\n'
open(p, 'w', encoding='utf-8').write(s)
EOP
  fi
  rm -rf "$NP/steamac/msvproc"
  echo "uninstalled"
  ;;
*) echo "usage: variant.sh status|install|uninstall" >&2; exit 1 ;;
esac
