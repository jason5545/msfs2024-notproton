#!/bin/bash
# make-proton-copy.sh <new winevulkan.so> <copy dir> — hard-link copy of the in-place "Proton 10.0" with only
# files/lib/wine/x86_64-unix/winevulkan.so replaced, for sharetest runs that must not touch the Proton the game uses.
set -eu
SRC="$HOME/.local/share/Steam/steamapps/common/Proton 10.0"
SO=$(realpath "$1"); DST=$2
[ -e "$DST" ] && { echo "$DST exists"; exit 1; }
cp -al "$SRC" "$DST"
T="$DST/files/lib/wine/x86_64-unix/winevulkan.so"
rm -f "$T"; cp "$SO" "$T"; chmod 555 "$T"
sha256sum "$SRC/files/lib/wine/x86_64-unix/winevulkan.so" "$T"
