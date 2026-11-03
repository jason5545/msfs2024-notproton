#!/bin/bash
# build-notproton.sh [stage|app|install] — builds our NotProton (fork jason5545/NotProton-steamac, branch steamac)
# with this project's components in it, instead of laying them over NotProton through variants and plugins.
#
#   stage    copy the components below into the fork's app payload (payload/steamac/, read by SteamacOverlay.swift)
#   app      stage, then `make app` signed with $NP_SIGN_ID (default: the first Apple Development identity)
#   install  app, then overwrite /Applications/NotProton.app with it, no backup (NotProton must not be running)
#
# The components (each built by its own tool in this repo) and where they go in the FEX runner's CrossOver folder:
#   FEX         $MSFS_SRC/fex-build/Bin/libarm64ecfex.dll            lib/wine/aarch64-windows/xtajit64.dll
#   FEX unixlib $MSFS_SRC/fex-unixlib-build/libarm64ecfex.so         lib/wine/aarch64-unix/libarm64ecfex.so
#               (tools/fex-unixlib/build.sh; hardware TSO. Replaces CrossOver's, whose call numbers differ from ours)
#   advapi32    $MSFS_SRC/wine-adv-build/out/advapi32.dll            lib/wine/aarch64-windows/advapi32.dll
#   msvproc     $MSFS_SRC/wine-vproc-build/out/msvproc.dll           lib/wine/aarch64-windows/msvproc.dll
#   Vulkan      $VK_LOADER (Homebrew vulkan-loader)                  lib/aarch64/libMoltenVK.dylib (CrossOver's
#   loader                                                           MoltenVK moves to lib/aarch64/moltenvk/)
#   KK          $MSFS_SRC/kk/stage-$KK_STAGE                         lib/steamac/kk/
#   DXVK        $MSFS_SRC/d3d-build/dxvk-ec/arm64ec/*.dll            lib/steamac/d3d/  (ARM64EC)
#   vkd3d       $MSFS_SRC/d3d-build/vkd3d-ec/arm64ec/*.dll           lib/steamac/d3d/  (ARM64EC)
# The runner files go in for runner build $NP_RUNNER_BUILD only. Bridge and fonts are taken from the installed app
# the first time (upstream's bridge needs a whole Wine tree); `make bridge`/`make fonts` in the fork rebuild them.
set -euo pipefail
MSFS_SRC=${MSFS_SRC:-$HOME/GitHub/msfs-src}
NP=${NP:-$MSFS_SRC/forks/NotProton-steamac}
NP_RUNNER_BUILD=${NP_RUNNER_BUILD:-27.0.0.41073-fex}
VK_LOADER=${VK_LOADER:-/opt/homebrew/opt/vulkan-loader/lib/libvulkan.1.dylib}
KK_STAGE=${KK_STAGE:-rel}
INSTALLED=/Applications/NotProton.app
PAYLOAD=$NP/app/Sources/NotProtonApp/Resources/payload
INSTALLED_PAYLOAD=$INSTALLED/Contents/Resources/NotProtonApp_NotProtonApp.bundle/Contents/Resources/payload

need() { [ -f "$1" ] || { echo "missing $1" >&2; exit 1; }; }
put() { # put <source> <path under steamac/runner>
  need "$1"; mkdir -p "$(dirname "$SC/runner/$2")"; cp -p "$1" "$SC/runner/$2"
  printf '%s  %s  <- %s\n' "$(shasum -a 256 "$1" | cut -d' ' -f1)" "$2" "${1/#$HOME/~}" >> "$SC/SOURCES"
}

stage() {
  SC=$PAYLOAD/steamac
  rm -rf "$SC"; mkdir -p "$SC/runner"
  echo "$NP_RUNNER_BUILD" > "$SC/build"
  : > "$SC/SOURCES"
  put "$MSFS_SRC/fex-build/Bin/libarm64ecfex.dll" lib/wine/aarch64-windows/xtajit64.dll
  put "$MSFS_SRC/fex-unixlib-build/libarm64ecfex.so" lib/wine/aarch64-unix/libarm64ecfex.so
  put "$MSFS_SRC/wine-adv-build/out/advapi32.dll" lib/wine/aarch64-windows/advapi32.dll
  put "$MSFS_SRC/wine-vproc-build/out/msvproc.dll" lib/wine/aarch64-windows/msvproc.dll
  put "$VK_LOADER" lib/aarch64/libMoltenVK.dylib
  put "$MSFS_SRC/kk/stage-$KK_STAGE/libvulkan_kosmickrisp.dylib" lib/steamac/kk/libvulkan_kosmickrisp.dylib
  put "$MSFS_SRC/kk/stage-$KK_STAGE/kosmickrisp_icd.json" lib/steamac/kk/kosmickrisp_icd.json
  local d
  for d in dxgi d3d11 d3d10core; do put "$MSFS_SRC/d3d-build/dxvk-ec/arm64ec/$d.dll" "lib/steamac/d3d/$d.dll"; done
  for d in d3d12 d3d12core; do put "$MSFS_SRC/d3d-build/vkd3d-ec/arm64ec/$d.dll" "lib/steamac/d3d/$d.dll"; done
  # what the stamp in the runner will say, so a reader can tell builds apart
  echo "staged into $SC (runner build $NP_RUNNER_BUILD):"; cat "$SC/SOURCES"

  # bridge and fonts: reuse the installed app's when the fork has none of its own yet
  if [ ! -f "$PAYLOAD/bridge/steam.exe" ]; then
    [ -d "$INSTALLED_PAYLOAD/bridge" ] || { echo "no bridge staged and no installed NotProton to take it from; run make bridge in $NP" >&2; exit 1; }
    mkdir -p "$PAYLOAD/bridge"; cp -p "$INSTALLED_PAYLOAD/bridge/"* "$PAYLOAD/bridge/"
    echo "bridge taken from $INSTALLED"
  fi
  if [ ! -d "$NP/build/fonts" ]; then
    [ -d "$INSTALLED_PAYLOAD/fonts" ] || { echo "no fonts built and no installed NotProton to take them from; run make fonts in $NP" >&2; exit 1; }
    mkdir -p "$NP/build/fonts"; rsync -a "$INSTALLED_PAYLOAD/fonts/" "$NP/build/fonts/"
    echo "fonts taken from $INSTALLED"
  fi
}

app() {
  stage
  local id=${NP_SIGN_ID:-}
  [ -n "$id" ] || id=$(security find-identity -v -p codesigning | awk '/Apple Development/ { print $2; exit }')
  [ -n "$id" ] || { echo "no Apple Development signing identity; set NP_SIGN_ID" >&2; exit 1; }
  (cd "$NP" && make app APP_SIGN_ID="$id") > "$MSFS_SRC/np-build.log" 2>&1 || { tail -40 "$MSFS_SRC/np-build.log"; exit 1; }
  tail -3 "$MSFS_SRC/np-build.log"
  codesign -dv "$NP/out/NotProton.app" 2>&1 | grep -E 'TeamIdentifier|Identifier='
}

install_app() {
  app
  if pgrep -xq NotProtonApp; then echo "NotProton is running; quit it first" >&2; exit 1; fi
  # No backup, no side copy (Jason 11/3): the app is rebuilt from the fork whenever needed.
  rm -rf "$INSTALLED"; ditto "$NP/out/NotProton.app" "$INSTALLED"
  codesign --verify --strict "$INSTALLED" && echo "installed $INSTALLED ($(git -C "$NP" rev-parse --short HEAD))"
}

case "${1:-app}" in
  stage) stage ;;
  app) app ;;
  install) install_app ;;
  *) echo "usage: $0 stage|app|install" >&2; exit 2 ;;
esac
