#!/bin/sh
# Runs d3d12gpu.exe under a runtime copy the way compat_run.sh sets up the vkd3d graphics option, in a
# throwaway prefix (never a game's). No window. Mesa's shader cache is off so every run compiles fresh
# (switches such as HB_KK_NO_UBO_ROBUST change the compiled shaders).
#   tools/gpu-bench/run.sh <runtime dir> <prefix dir (absolute)> <out dir> <d3d12gpu args...>
set -eu
RT=$1; PFX=$2; OUT=$3; shift 3
here=$(cd "$(dirname "$0")" && pwd)
HADRON_DLL_OVERLAY="$RT/lib/graphics/vkd3d:$RT/lib/graphics/dxvk" \
VK_DRIVER_FILES="$RT/share/vulkan/icd.d/kosmickrisp_icd.json" \
VKD3D_FEATURE_LEVEL=12_1 VKD3D_SHADER_MODEL=6_7 HADRON_VK_HOST_SHARING=1 HADRON_MAC_BACKGROUND=1 \
MESA_SHADER_CACHE_DISABLE=true TMO=${TMO:-300} \
  "$here/../hadron/run.sh" "$RT" "$PFX" "$OUT" "$here/build/d3d12gpu.exe" "$@"
cat "$OUT/stdout.txt"
grep -v '^\s*$' "$OUT/stderr.txt" | grep -v 'fixme' | head -20 || true
grep exit= "$OUT/run.txt"
