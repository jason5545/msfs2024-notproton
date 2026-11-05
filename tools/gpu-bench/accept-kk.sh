#!/bin/bash
# Acceptance for a KosmicKrisp change, offline: d3d12test (vkd3d-proton, background mode, winwatch watching)
# and vktest under a runtime copy, then vkrov (native Vulkan, VK_EXT_fragment_shader_interlock, N=1024,
# storage image and storage buffer) against the runtime's KosmicKrisp. Prints one line per test.
#   tools/gpu-bench/accept-kk.sh <runtime dir> <prefix dir (absolute)> <out dir>
# VKROV=<folder with vkrov, lock.spv, nolock.spv> (default ~/GitHub/msfs-src/gpu-1105/vkrov).
set -u
RT=$(cd "$1" && pwd); PFX=$2; OUT=$3
T=$(cd "$(dirname "$0")/.." && pwd)
VKROV=${VKROV:-$HOME/GitHub/msfs-src/gpu-1105/vkrov}
mkdir -p "$OUT"
export HADRON_DLL_OVERLAY="$RT/lib/graphics/vkd3d:$RT/lib/graphics/dxvk"
export VK_DRIVER_FILES="$RT/share/vulkan/icd.d/kosmickrisp_icd.json"
export VKD3D_FEATURE_LEVEL=12_1 VKD3D_SHADER_MODEL=6_7 HADRON_VK_HOST_SHARING=1 HADRON_MAC_BACKGROUND=1
"$T/video/winwatch" 120 "$RT" "$OUT/winwatch.log" > /dev/null 2>&1 &
ww=$!
for t in d3d12test vktest; do
  TMO=120 "$T/hadron/run.sh" "$RT" "$PFX" "$OUT/$t" "$T/np-gfx/build/$t.exe" > /dev/null 2>&1
  echo "$t $(grep -h 'exit=' "$OUT/$t/run.txt") $(grep -h 'RESULT' "$OUT/$t/stdout.txt" | tr '\n' ' ')"
done
kill $ww 2>/dev/null; wait $ww 2>/dev/null
echo "winwatch $(grep -c -E 'LEAK|STOLE' "$OUT/winwatch.log" 2>/dev/null) LEAK/STOLE lines"
cd "$VKROV"
for spv in lock.spv nolock.spv; do
  VK_DRIVER_FILES="$RT/share/vulkan/icd.d/kosmickrisp_icd.json" ./vkrov "$spv" 1024 2>&1 | grep -E 'N=|FAIL' | sed 's/^/vkrov /'
done | tee "$OUT/vkrov.txt"
