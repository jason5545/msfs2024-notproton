# gpu-bench

離線量 D3D12 → vkd3d-proton → KosmicKrisp 這條路的 GPU 時間，不開模擬器（紀錄 25.56）。

- `d3d12gpu.c`：沒有視窗的 D3D12 小程式，用 D3D12 時間戳量 GPU 毫秒，每次印中位數、最小值和輸出貼圖的 checksum（換版本比對正確性）。模式：
  - `cb [iters]`：constant buffer 讀取（descriptor table 的 CBV，robustness 管到的那條），一半固定索引一半每個像素不同；接著檢查兩種越界：256 B 的 CBV 讀第 16–19 列、null CBV。
  - `cbfix [iters]`：同上，但全是固定位移（遊戲 shader 常數的典型讀法），數值精確，任何版本 checksum 都該相同。
  - `rt <format> [splits] [smooth|noise]`：每段一個 G-buffer pass（4 張 RT＋D32）加一個讀這 4 張的 pass，量 RT 的存取頻寬。格式 `rgba8`、`rgba8srgb`、`rgba8tl`（TYPELESS）、`rgb10a2`、`rg16`、`rgba16f`。
  - `rtcast`：在 R8G8B8A8_UNORM 的 RT 上開 UNORM_SRGB 和 UINT 兩個 SRV，看驅動對轉型 view 說什麼。
- `build.sh`：用 Homebrew 的 mingw-w64 編。
- `run.sh <runtime> <prefix 絕對路徑> <輸出資料夾> <模式和參數…>`：照 `compat_run.sh` 選 vkd3d 的環境跑，Mesa shader 快取關掉（`HB_KK_NO_UBO_ROBUST` 這類開關會改編出來的 shader）。runtime 用 `hadron-test` 裡的複本，prefix 用一次性的。
- `accept-kk.sh <runtime> <prefix> <輸出>`：改 KosmicKrisp 之後的回歸：`tools/np-gfx` 的 d3d12test（背景模式，winwatch 盯著）、vktest，加上 vkrov N=1024（有／沒有 interlock，影像和 buffer）。vkrov 在 `~/GitHub/msfs-src/gpu-1105/vkrov`（`VKROV=` 可改）。

Shader 是執行時用 Wine 內建的 d3dcompiler_47 編的 DXBC（SM 5），模擬器用的是 DXIL（SM 6，經 dxil-spirv）。兩條路的 constant buffer 都變成 Vulkan UBO、RT 都經 vkd3d-proton 建 image，所以量到的是同一段 KosmicKrisp 程式。
