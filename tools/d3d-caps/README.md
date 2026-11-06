# d3d-caps

用來量 medium 對 ultra 的 GPU 那一側，不開模擬器（紀錄 25.53）。

- `d3d12caps.c`：查 vkd3d-proton＋KosmicKrisp 回報的 D3D12 功能（feature level、shader model、raytracing、mesh shader、VRS、sampler feedback），試 GPU 時間戳（direct、copy queue）和 `QueryVideoMemoryInfo`。`build.sh` 用 Homebrew 的 mingw-w64 編。
- `mst_gpu.py <trace> [pid]`：把 Instruments 的 Metal System Trace（`xcrun xctrace record --template 'Metal System Trace' --attach <pid>`）換成每幀 GPU 毫秒。
- `mst_gaps.py <metal-gpu-intervals.xml> <pid> [present 次數]`：從 Metal System Trace 匯出的 GPU intervals 算一個程序的 GPU 空檔：大小分布、各段佔空檔多少、空檔前後的 command buffer 編號有沒有變（在 command buffer 之間還是裡面；KK 用 MTL4，編號會重複用，只能比相鄰兩個），以及 command buffer 內部 40–160 µs 空檔前後的 encoder 類型（紀錄 25.58）。
- `mst_encgaps.py <metal-gpu-intervals.xml> <pid> [幀數]`：同一份 intervals，看空檔裡夾了什麼（紀錄 25.59）。每段空檔大小列出前後 encoder 編號跳幾號：同一個 command buffer 裡相鄰的 encoder 編號連號，跳 2 以上表示中間有沒留下區間的東西（KosmicKrisp 的 timestamp counter heap resolve 就是）；40–160 µs 的空檔再按前後區間分類：前面是小 Compute（< 6 µs，像 KK 寫 timestamp 前的 `write_u32`）、後面是 Vertex（list 開頭的 timestamp，`patches/kk/0014` 省得掉），或後面是 command buffer 最後一個小 Compute（list 結尾 resolve 完的 query copy，省不掉）。

跑法照 `compat_run.sh` 選 vkd3d 時的環境，用 `tools/hadron/run.sh` 在一次性 prefix 跑：

```sh
RT=<runtime 複本>
HADRON_DLL_OVERLAY="$RT/lib/graphics/vkd3d:$RT/lib/graphics/dxvk" \
VK_DRIVER_FILES="$RT/share/vulkan/icd.d/kosmickrisp_icd.json" \
VKD3D_FEATURE_LEVEL=12_1 VKD3D_SHADER_MODEL=6_7 HADRON_VK_HOST_SHARING=1 HADRON_MAC_BACKGROUND=1 \
  tools/hadron/run.sh "$RT" <prefix 的絕對路徑> <輸出資料夾> tools/d3d-caps/build/d3d12caps.exe
```

prefix 一定要寫絕對路徑，不然 Wine 拒絕。有視窗的測試（`tools/np-gfx/d3d12test.exe`，`D3D12TEST_FRAMES` 調幀數）要在背景跑就帶 `HADRON_MAC_BACKGROUND=1`（11/6 起自己寫的小程式任何時間都可以在前景跑，只有模擬器在上班時間不准出現在畫面上），用 `tools/video/winwatch <秒> <runtime 路徑片段> <log>` 盯著；winwatch 比對的是執行檔路徑，樣式寫 runtime 的資料夾，不是 exe 名稱。
