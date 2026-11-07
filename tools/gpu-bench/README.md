# gpu-bench

離線量 D3D12 → vkd3d-proton → KosmicKrisp 這條路的 GPU 時間，不開模擬器（紀錄 25.56）。

- `d3d12gpu.c`：沒有視窗的 D3D12 小程式，用 D3D12 時間戳量 GPU 毫秒，每次印中位數、最小值和輸出貼圖的 checksum（換版本比對正確性）。模式：
  - `cb [iters]`：constant buffer 讀取（descriptor table 的 CBV，robustness 管到的那條），一半固定索引一半每個像素不同；接著檢查兩種越界：256 B 的 CBV 讀第 16–19 列、null CBV。
  - `cbfix [iters]`：同上，但全是固定位移（遊戲 shader 常數的典型讀法），數值精確，任何版本 checksum 都該相同。
  - `rt <format> [splits] [smooth|noise]`：每段一個 G-buffer pass（4 張 RT＋D32）加一個讀這 4 張的 pass，量 RT 的存取頻寬。格式 `rgba8`、`rgba8srgb`、`rgba8tl`（TYPELESS）、`rgb10a2`、`rg16`、`rgba16f`。
  - `rtcast`：在 R8G8B8A8_UNORM 的 RT 上開 UNORM_SRGB 和 UINT 兩個 SRV，看驅動對轉型 view 說什麼。
  - `stagger [frames] [lists] [iters] [two|one]`：一條 DIRECT、一條 COMPUTE queue 每幀各送 `lists` 個 command list（各一個 dispatch，`iters` 調工作量），DIRECT 用 fence 等 COMPUTE，CPU 最多讓 2 幀在路上（紀錄 25.58）。這是 vkd3d-proton 在只有一個 queue family 的裝置上打開 staggered submissions 的條件。印每幀的牆上時間、GPU 工作時間（時間戳）、GPU 忙碌比例和兩個 buffer 的 checksum。`one` 是對照組：全部送 DIRECT，COMPUTE 不用。
  - `gaps <變化> [n] [frames] [iters] [groups]`：一幀 n（64）個很小的 dispatch（`iters` 100、`groups` 4 時約 2.9 µs），每幀 Reset 後重錄，CPU 最多讓 2 幀在路上；幀頭幀尾的時間戳量 GPU span，印每個 dispatch 的 frame 和 GPU 微秒、checksum（每種變化的 checksum 都一樣）（紀錄 25.59）。變化：`nobar`（不放 barrier，時間戳量不準）、`uav`（每個後面一個 UAV barrier）、`gfx`（dispatch 和一個小 draw 交替，compute 和 render encoder 輪流）、`ts`（每個後面一個 timestamp，都在同一個 encoder）、`tsgfx`（gfx，dispatch 和 draw 之間一個 timestamp）、`tsdraw`（gfx，draw 後面、render pass 裡一個 timestamp）、`lists`（n 個 list 各一個 dispatch，一次 ExecuteCommandLists）、`tslists`（lists，每個 list 一個 timestamp）、`exec`（n 個 list，n 次 ExecuteCommandLists）、`tsresolve`（n 個 list，每個照模擬器裡被量時間的 pass：timestamp、dispatch、draw、render pass 裡的 timestamp、ResolveQueryData）、`tsrescs`（tsresolve 沒有 draw）。
  - `cpool [lists] [allocs] [frames] [draws]`：照模擬器每幀約 129 個帶 timestamp 的小 list（紀錄 25.72）。3 幀在路上，每幀每個 allocator（`allocs`，1）先 Reset，再從它錄 `lists`（32）個 list，每個：timestamp、`draws`（8）個帶 root constant 的一個三角形、timestamp、ResolveQueryData；一次 ExecuteCommandLists。印每幀 Reset、錄製、ExecuteCommandLists、幀時間的中位數和 p90，整段的程序 CPU（Wine 的計數 10 ms 一格，所以只算整段平均），每個 timestamp 對都檢查非零、後者較大。幀時間在這支小程式裡是 GPU 決定的（timestamp 的 resolve，25.59），比 CPU 看前面那幾欄。
  - `tscheck [frames] [iters]`：時間戳的值對不對（紀錄 25.59）。每幀一個 list：時間戳包住 dispatch A、ResolveQueryData；同一組 query 再寫一次（vkd3d-proton 會在 list 裡 reset）包住兩倍長的 dispatch B、ResolveQueryData；再一對包住 draw（後一個在 render pass 裡）。每幀檢查全部非零、單調、B/A 在 1.5–2.5，印中位數和 PASS／FAIL。
- `build.sh`：用 Homebrew 的 mingw-w64 編。
- `run.sh <runtime> <prefix 絕對路徑> <輸出資料夾> <模式和參數…>`：照 `compat_run.sh` 選 vkd3d 的環境跑，Mesa shader 快取關掉（`HB_KK_NO_UBO_ROBUST` 這類開關會改編出來的 shader）。runtime 用 `hadron-test` 裡的複本，prefix 用一次性的。
- `stagger.sh <runtime> <prefix> <輸出>`：`stagger` 模式的 A/B，A 是預設、B 加 `VKD3D_CONFIG=no_staggered_submit`，每個點交替跑 `ROUNDS`（3）輪，掃 `LISTS`×`ITERS`×`MODES`，開 `VKD3D_DEBUG=info` 數「Enabling staggered」。結果在 `<輸出>/summary.tsv`，`stagger_sum.py summary.tsv` 印每個點的中位數、最小值、B/A、checksum 是否一致。
- `gaps-ab.sh <runtime A> <runtime B> <prefix> <輸出>`：`gaps` 的 A/B（紀錄 25.59）：a 是 runtime A、b 是 runtime B、c 是 runtime B 加 `C_ENV`（預設 `MESA_KK_EAGER_TS_RESOLVE=1`，KK 0014 的退回開關），每種變化交替跑 `ROUNDS`（3）輪，`VARIANTS`、`ARMS`、`N`、`FRAMES`、`ITERS`、`GROUPS` 可改。`gaps_sum.py summary.tsv` 印每種變化各組的 GPU span 中位數（最小值）、frame、b/a、checksum 是否一致。
- `cpool-ab.sh <runtime A> <runtime B> <prefix> <輸出> [rounds] [frames] [形狀…]`：`cpool` 的 A/B（紀錄 25.72）：a 是 runtime A、b 是 runtime B、c 是 runtime B 加 `MESA_KK_CMD_POOL_FIXED_CAP=1`（KK 0015 的退回開關），形狀寫成 `lists:allocs`（預設 `8:1 32:1 64:1 128:1 32:4 8:16`），交替跑 `rounds`（3）輪，每筆一行。`cpool_sum.py <那個輸出>` 印每種形狀各組三輪的中位數、b/a、c/a。
- `gaps-trace.sh <runtime> <prefix> <輸出> <名字> <gaps 參數…>`：背景跑 `gaps`，2 秒後用 Instruments 的 GPU instrument 錄 3 秒（`WAIT`、`LIMIT`），匯出 `metal-gpu-intervals`、記下 pid，給 `tools/d3d-caps/mst_encgaps.py` 用。trace 和匯出的 XML 很大，放 repo 外面。錄 trace 會把空檔放大（同一個 tsgfx：沒錄時每對多 31 µs，錄的時候空檔中位 79 µs），A/B 以沒錄的 `gaps-ab.sh` 為準。
- `accept-kk.sh <runtime> <prefix> <輸出>`：改 KosmicKrisp 之後的回歸：`tools/np-gfx` 的 d3d12test（背景模式，winwatch 盯著）、vktest，加上 vkrov N=1024（有／沒有 interlock，影像和 buffer）。vkrov 在 `~/GitHub/msfs-src/gpu-1105/vkrov`（`VKROV=` 可改）。

Shader 是執行時用 Wine 內建的 d3dcompiler_47 編的 DXBC（SM 5），模擬器用的是 DXIL（SM 6，經 dxil-spirv）。兩條路的 constant buffer 都變成 Vulkan UBO、RT 都經 vkd3d-proton 建 image，所以量到的是同一段 KosmicKrisp 程式。
