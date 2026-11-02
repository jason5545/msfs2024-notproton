#!/bin/bash
# run-np.sh <exe> <outdir> [args...] — runs a Windows test program on macOS through a COPY of the NotProton
# runner (CrossOver 27 Preview Wine + steamac FEX + Vulkan loader), with vkd3d-proton/DXVK next to the exe and
# KosmicKrisp as the only Vulkan driver. Never points at NotProton's own runner, prefix or Steam's compatdata.
#
#   CX_ROOT  runner copy (the CrossOver dir), default ~/GitHub/msfs-src/np-video/runner
#   PFX      test prefix, default ~/GitHub/msfs-src/np-video/pfx (created with wineboot on first use)
#   KK_ICD   KosmicKrisp ICD json, default ~/GitHub/msfs-src/np-video/kk-197/kosmickrisp_icd.json
#   D3D      folder with d3d12/d3d12core (vkd3d-proton) and dxgi/d3d11/d3d10core (DXVK),
#            default ~/GitHub/msfs-src/np-video/d3d-np (the set NotProton's NOTPROTON_STEAMAC plugin stages)
#   TMO      timeout in seconds (default 120); WINEDEBUG, DXVK_LOG_LEVEL, VKD3D_DEBUG pass through
set -u
EXE=$1; OUT=$2; shift 2
NV=$HOME/GitHub/msfs-src/np-video
CX_ROOT=${CX_ROOT:-$NV/runner}
export WINEPREFIX=${PFX:-$NV/pfx}
case "$CX_ROOT$WINEPREFIX" in *"Application Support/notproton"*|*compatdata*) echo "refusing NotProton/Steam paths" >&2; exit 2 ;; esac
export CX_ROOT CX_HOME="$HOME/Library/Application Support/CrossOver"
wine_unix="$CX_ROOT/lib/wine/aarch64-unix"
export WINELOADER="$wine_unix/wine.app/Contents/MacOS/wine" WINESERVER="$CX_ROOT/bin/wineserver-arm64"
export WINEDLLPATH="$CX_ROOT/lib/wine/x86_64-windows:$wine_unix" PATH="$CX_ROOT/bin:$PATH"
export WINEDEBUG=${WINEDEBUG:--all}  # CW 27 Wine faults inside vkGetPhysicalDeviceProperties2 when default err/fixme channels print (d3d12test, 11/2)
export VK_DRIVER_FILES=${KK_ICD:-$NV/kk-197/kosmickrisp_icd.json} VK_LOADER_DEBUG=${VK_LOADER_DEBUG:-error,warn}
export WINEDLLOVERRIDES="mscoree,mshtml=;d3d12,d3d12core,dxgi,d3d11,d3d10core=n,b"
export VKD3D_FEATURE_LEVEL=12_1 VKD3D_SHADER_MODEL=6_7 VKD3D_DEBUG=${VKD3D_DEBUG:-warn} DXVK_LOG_LEVEL=${DXVK_LOG_LEVEL:-info}
D3D=${D3D:-$NV/d3d-np}
mkdir -p "$OUT"; cp "$EXE" "$OUT/"
cp "$D3D"/d3d12.dll "$D3D"/d3d12core.dll "$D3D"/dxgi.dll "$D3D"/d3d11.dll "$D3D"/d3d10core.dll "$OUT/"
cd "$OUT"
{ echo "runner=$CX_ROOT"; echo "prefix=$WINEPREFIX"; echo "exe=$EXE $*"; echo "WINEDLLOVERRIDES=$WINEDLLOVERRIDES"
  env | grep -E '^(VK_|VKD3D_|DXVK_|WINEDEBUG|MESA_)' | sort
  shasum -a 256 "$(dirname "$VK_DRIVER_FILES")"/libvulkan_kosmickrisp.dylib ./*.dll "$wine_unix/winevulkan.so" "$wine_unix/win32u.so"
  date '+start %T'; } > run.txt
[ -f "$WINEPREFIX/system.reg" ] || "$WINELOADER" wineboot --init > boot.txt 2>&1
date '+booted %T' >> run.txt
timeout "${TMO:-120}" "$WINELOADER" "$OUT/$(basename "$EXE")" "$@" > stdout.txt 2> stderr.txt
echo "exit=$?" >> run.txt
date '+end %T' >> run.txt
"$WINESERVER" -k 2>/dev/null; "$WINESERVER" -w 2>/dev/null
date '+done %T' >> run.txt
