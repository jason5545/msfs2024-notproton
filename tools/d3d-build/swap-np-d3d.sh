#!/bin/bash
# swap-np-d3d.sh status|hostshare|ec|restore — swaps the 5 D3D DLLs that NotProton's NOTPROTON_STEAMAC plugin stages
# into the MSFS prefix (logs/notproton-log.md 14.7). NotProton copies the stage folder into system32 on every
# launch when the files differ, so this only takes effect at the next game start; the prefix is not touched.
#
#   status     sha256 of the stage folder and which known set it is
#   hostshare  back up the stage folder, then install DXVK 68b7647d + vkd3d-proton eacf43b9 (host-memory sharing)
#   ec         same sources as hostshare (+ DXVK 4286731d, a libc++ build fix) built as ARM64EC with llvm-mingw: the
#              game still loads them as x64 DLLs, but their code runs natively instead of inside FEX (log section 22)
#   restore    put back the original set (DXVK 3cb664e1 + vkd3d-proton 3.0b, the game's set since T5)
#
# Every copy is checked against the sha256 below before and after; anything unexpected stops the script.
set -euo pipefail
STAGE="$HOME/Library/Application Support/notproton/steamac/d3d"
NEW=$HOME/GitHub/msfs-src/np-video/d3d-host
ORIG=$HOME/GitHub/msfs-src/np-video/d3d-np
EC=$HOME/GitHub/msfs-src/np-video/d3d-ec
BACKUP=$HOME/GitHub/msfs-src/backup
DLLS="dxgi d3d11 d3d10core d3d12 d3d12core"

sha_orig() { case $1 in
  dxgi) echo aa9308a7c097eca0ba95bbf9c63ff4badfa204bafa63738aee7990e2067c8d1e ;;
  d3d11) echo 10191289f163c49d71c89fd0149911d92307b9a645516fdafed2892001bba0e3 ;;
  d3d10core) echo 4fa444af376cab7f62f92f6abd77f542fc55f00da51eaef22c04d4d26d80141b ;;
  d3d12) echo 3575e61184d23d00f2929cab1ab89791fb7becfbd43f31f644e7f49c3b30bea6 ;;
  d3d12core) echo 36df310e9d611725249204649a83f446e9a8a6cc815ad910a805a321cf6e9822 ;;
esac; }
sha_new() { case $1 in
  dxgi) echo d0eee660ebe42271d294f54ce82b0766cb26dfcc75f002dc125fd95bade669e6 ;;
  d3d11) echo 601411a11b05d49e555b7df0590d456196847a732bd3c53bf1cfdd314a7f200f ;;
  d3d10core) echo 2ddbd515699179427feb823d17b6983a60562988022025c02b9e807af6065280 ;;
  d3d12) echo 395fa4f357f5a2d30f5ae86ae39d2e337bd53c13b6b6fe7166a9bf34299c2807 ;;
  d3d12core) echo 252e98c39146049adeebc926d2c9e7ca183f98019ac684439653fe9d95251a32 ;;
esac; }
sha_ec() { case $1 in
  dxgi) echo b5b519e619145c18f7a8b781814d0310fca44222babf435b5ed653d0f513f6f0 ;;
  d3d11) echo d75cc664f11a853af8dc89a4d28d61f7074bde393617b385aab3f3fbb7d39a03 ;;
  d3d10core) echo 812ad3d253c68ea715e6424cff4e873403bb42b30daf7ff74f7a50deee9c1f77 ;;
  d3d12) echo 624488f6f694ee00bac1ecdd802b02b8348b6c659ba296f84f0c2c1ccc26b963 ;;
  d3d12core) echo 62126809d54924bd390702f44ce75587f576c5d1312a82e3abff664141a0150d ;;
esac; }
sha() { shasum -a 256 "$1" | cut -d' ' -f1; }

which_set() {
  local o=1 n=1 e=1 d
  for d in $DLLS; do
    local s; s=$(sha "$STAGE/$d.dll")
    [ "$s" = "$(sha_orig $d)" ] || o=0
    [ "$s" = "$(sha_new $d)" ] || n=0
    [ "$s" = "$(sha_ec $d)" ] || e=0
  done
  if [ $o = 1 ]; then echo original; elif [ $n = 1 ]; then echo hostshare; elif [ $e = 1 ]; then echo ec; else echo unknown; fi
}

check_dir() { # check_dir <dir> <sha function>
  local d
  for d in $DLLS; do
    [ "$(sha "$1/$d.dll")" = "$($2 $d)" ] || { echo "$1/$d.dll does not match the expected sha256" >&2; exit 1; }
  done
}

install_set() { # install_set <dir> <sha function> <name>
  check_dir "$1" "$2"
  pgrep -fq 'FlightSimulator2024' && echo "note: MSFS is running; the new DLLs apply from the next launch"
  local b; b="$BACKUP/d3d-stage-$(date +%m%d-%H%M%S)"
  mkdir -p "$b"; cp -p "$STAGE"/*.dll "$b/"; (cd "$b" && shasum -a 256 *.dll > SHA256SUMS)
  echo "backed up the stage folder ($(which_set)) to $b"
  local d
  for d in $DLLS; do cp -p "$1/$d.dll" "$STAGE/$d.dll.swap.$$" && mv -f "$STAGE/$d.dll.swap.$$" "$STAGE/$d.dll"; done
  check_dir "$STAGE" "$2"
  echo "stage folder is now: $3"
}

case "${1:-status}" in
  status) for d in $DLLS; do printf '%-14s %s\n' "$d.dll" "$(sha "$STAGE/$d.dll")"; done; echo "set: $(which_set)" ;;
  hostshare) install_set "$NEW" sha_new hostshare ;;
  ec) install_set "$EC" sha_ec ec ;;
  restore) install_set "$ORIG" sha_orig original ;;
  *) echo "usage: $0 status|hostshare|ec|restore" >&2; exit 2 ;;
esac
