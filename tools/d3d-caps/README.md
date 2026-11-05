# d3d-caps

用來量 medium 對 ultra 的 GPU 那一側，不開遊戲（紀錄 25.53）。

- `d3d12caps.c`：查 vkd3d-proton＋KosmicKrisp 回報的 D3D12 功能（feature level、shader model、raytracing、mesh shader、VRS、sampler feedback），試 GPU 時間戳（direct、copy queue）和 `QueryVideoMemoryInfo`。`build.sh` 用 Homebrew 的 mingw-w64 編。
- `mst_gpu.py <trace> [pid]`：把 Instruments 的 Metal System Trace（`xcrun xctrace record --template 'Metal System Trace' --attach <pid>`）換成每幀 GPU 毫秒。

跑法照 `compat_run.sh` 選 vkd3d 時的環境，用 `tools/hadron/run.sh` 在一次性 prefix 跑：

```sh
RT=<runtime 複本>
HADRON_DLL_OVERLAY="$RT/lib/graphics/vkd3d:$RT/lib/graphics/dxvk" \
VK_DRIVER_FILES="$RT/share/vulkan/icd.d/kosmickrisp_icd.json" \
VKD3D_FEATURE_LEVEL=12_1 VKD3D_SHADER_MODEL=6_7 HADRON_VK_HOST_SHARING=1 HADRON_MAC_BACKGROUND=1 \
  tools/hadron/run.sh "$RT" <prefix 的絕對路徑> <輸出資料夾> tools/d3d-caps/build/d3d12caps.exe
```

prefix 一定要寫絕對路徑，不然 Wine 拒絕。有視窗的測試（`tools/np-gfx/d3d12test.exe`，`D3D12TEST_FRAMES` 調幀數）一律帶 `HADRON_MAC_BACKGROUND=1`，用 `tools/video/winwatch <秒> <runtime 路徑片段> <log>` 盯著；winwatch 比對的是執行檔路徑，樣式寫 runtime 的資料夾，不是 exe 名稱。
