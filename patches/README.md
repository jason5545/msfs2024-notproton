# patches

`kk/`、`dxvk/`、`vkd3d-proton/` 是 Hadron 路線照舊在用的（KosmicKrisp 和 ARM64EC 的 D3D 層，`tools/hadron/build-runtime.sh` 帶進 runtime）。`wine/`、`fex/`、`notproton/` 是 NotProton 路線（11/3 中午之前）用的，停用、留著當紀錄：Hadron 路線的 Wine 和 FEX patch 在 fork `jason5545/Hadron` 的 `runtime/`。

| 檔案 | 套在哪 | 內容 |
|---|---|---|
| `wine/advapi32-steamac.patch` | 上游 Wine 11.17（`36b6a2c`） | `CredEnumerateW` 把宿主回的 `ERROR_NOT_FOUND` 也當成沒有宿主憑證；`GetUserNameA/W` 照 CrossOver Hack 12735 回 `crossover` |
| `wine/msvproc-arm64ec-yuv2rgb.patch` | 上游 Wine 11.17（`36b6a2c`） | 內建 FFmpeg 的 ARM64EC `yuv2rgb_neon.S`：先收窄 B1／B2，避免後 8 個像素的紅色蓋掉前 8 個的藍色 |
| `dxvk/0001-…` | DXVK `3cb664e1`（v2.6.2-23） | D3D11 共用貼圖改走宿主記憶體，不靠 Win32 handle |
| `dxvk/0002-…` | 接在 0001 後面 | 讓 libc++（llvm-mingw，編 ARM64EC 用）編得過：`unordered_map::emplace` 的空 key tuple 改成直接傳預設 key。缺的 `<algorithm>`、`<new>`、`<exception>` 由 `tools/d3d-build/cross-arm64ec.txt` 強制 include，不改子模組 |
| `vkd3d-proton/0001-…` | vkd3d-proton `21a49c97`（v3.0b） | 同上，D3D12 那一側 |
| `vkd3d-proton/0002-…` | 接在 0001 後面 | `VKD3D_SQUARK_RT_NOCAST=1`（預設關）：不是 TYPELESS、不是 UAV、沒指定 castable 格式的 render target，格式清單只留自己和 sRGB 雙胞胎，不再放 D3D12 轉型規則允許的 UINT／SINT 等。配合 `kk/0013`，R8G8B8A8_UNORM、R10G10B10A2_UNORM 這類 RT 拿回 Apple 的無損壓縮（離線 −40%，紀錄 25.56）。程式真的開了轉型 view 時，KosmicKrisp 會印 `kk: view as …` |

| `notproton/0001-…` | NotProton `4df944f`（上游 main，11/3） | 把 payload 的 `steamac/`（`tools/np-build/build-notproton.sh` 收進去）寫進 FEX runner 並驗證（`SteamacOverlay.swift`）；圖形後端多一個「vkd3d-proton + KosmicKrisp」（`CX_GRAPHICS_BACKEND=vkd3d`）；其他 FEX 遊戲的 Vulkan 經 loader 照舊走 MoltenVK；元件換了就讓 prefix 重跑 wine.inf；關掉 Sparkle 自動更新。等於 fork `jason5545/NotProton-steamac` 分支 `steamac` 的 `227fd2f` |

編法見 `tools/wine-advapi32/build.sh`、`tools/wine-msvproc/`、`tools/d3d-build/build-d3d.sh`（`ARCH=arm64ec` 編 ARM64EC）。

## kk/

套在 Mesa `ce576c29ed` 加 fxgl/steamac v1.9.7 的 KosmicKrisp patch 0001–0077 之後：`tools/kk-build/build-kk.sh prepare $PWD/patches/kk/00*.patch`（0001–0015 依序，路徑要寫絕對路徑）。`build-kk.sh` 另外把 Mesa 磁碟 shader 快取的預設上限編成 8 GB（`-Dshader-cache-max-size`，Mesa 原本 1 GB，MSFS 11/4 塞滿過；環境變數 `MESA_SHADER_CACHE_MAX_SIZE` 照樣能蓋）。

| 檔案 | 內容 |
|---|---|
| `0001-…fragment-shader-interlock…` | D3D12 的 ROV（vkd3d-proton 轉成 `SPV_EXT_fragment_shader_interlock`）改用 Metal 的 raster order group。用到 interlock 的 fragment shader，每種可寫貼圖型別多一個 `[[buffer(3+N)]]` 參數，device 記憶體多一個 `[[buffer(11), raster_order_group(0)]]` 參數，所有存取的位址都從這些參數推出來。並宣告 `VK_EXT_fragment_shader_interlock`。原本 `begin/end_invocation_interlock` 會印成 `Unknown intrinsic`，Metal 編譯失敗，`vkEndCommandBuffer` 回 `VK_ERROR_INVALID_SHADER_NV`，接著 `vkQueueSubmit2` 崩潰 |
| `0002-…drop-the-draw…` | 背景編譯失敗的 pipeline 只丟掉那次繪圖，不再讓 `vkEndCommandBuffer` 回錯誤（vkd3d-proton 會照樣送出這種 command buffer，在 `vkQueueSubmit2` 崩潰） |
| `0003-…keep-the-MSL…` | Metal 編譯失敗時把 MSL 和錯誤存到 `~/Library/Logs/kk-failed-msl/`（最多 200 份；0005 之後要設 `MESA_KK_DUMP_FAILED_MSL=1`） |
| `0004-…PrimitiveShadingRateKHR…` | 拿掉 vertex shader 的 `PrimitiveShadingRateKHR` 輸出：dxil-spirv 看到 `SV_ShadingRate` 就會宣告，KK 沒有可變速率著色、MSL 也沒有這個輸出名稱，原本印成 `(null)` 編不過（MSFS 主選單 12 個 shader）。沒有 VRS 本來就是全速率著色 |
| `0005-…only-on-request` | 除錯輸出預設關閉：steamac 0073 的 encoder 診斷原本在 Apple9 以上的 GPU 自動打開（M5 Max 每秒約 500 行，佔 Wine log 94%），改成要設 `MESA_KK_ENCODER_LOG=1`；0003 的 MSL 保存要設 `MESA_KK_DUMP_FAILED_MSL=1` |
| `0006`–`0010` | Highball（highball-engine `kosmickrisp-arm64/highball/0001`–`0005`，`34bd3b0`）原樣搬來：sparse 資源可用 CPU 看得到的記憶體、產生的 MSL 壓縮並共用相同內容（Forza Horizon 6 的世界載入 19.5 GB → 4 GB）、壓縮貼圖不要求 shader 寫入、可寫 sparse 貼圖先排佇列分隔再對應、`HB_PRESENT_PACE_FPS` 平均送幀（預設不開）。0007 改 MSL 字串擁有權，steamac 0050–0077 和我們 0003 碰到這個字串的地方都核對過 |
| `0011-…lighter-encoder-end-barriers…` | Highball 的 `0007`，兩處上下文手動放（`u_debug.h` include、`null_ubo` 釋放移到 `vk_pipeline_cache_destroy` 之前），程式碼是 Highball 的。**改預設**：encoder 結尾的 barrier 要 Device 可見性、不再用 ResourceAlias（Forza GPU 時間 −9%），除非 render encoder 跑過 occlusion query；`HB_KK_ALIAS_BARRIERS=1` 換回舊的。`HB_KK_NO_UBO_ROBUST=1` 預設不開。Highball 的 `0006`（磁碟快取）沒搬：steamac 0055 已經有，還多算了 GPU 家族、MSL 版本、workaround |
| `0012-…robust-uniform-buffer-loads-per-16-byte-row` | 我們的。robustness 開著時（vkd3d-proton 一定開），每個 vec4 的 constant buffer 讀取原本被 NIR 拆成 4 個純量讀取、每個帶條件。KK 回報的 `robustUniformBufferAccessSizeAlignment` 是 64，所以不跨 16 B 列的讀取檢查起點一次就合規：新的 `nir_shader_compiler_options::bounded_ubo_row_loads` 讓這種讀取保持完整；越界時改讀 root 表尾端永遠是零的 32 B（`kk_root_descriptor_table::zero`），讀取本身不再帶條件。越界和 null CBV 照樣讀到 0。離線：動態索引 4.4 → 2.5 ms、固定位移 22.0 → 16.7–17.4 ms（`HB_KK_NO_UBO_ROBUST=1` 完全不檢查是 1.8、15.2–15.6），紀錄 25.56 |
| `0013-…no-PixelFormatView-for-format-lists-of-sRGB-twins` | 我們的。`VK_IMAGE_CREATE_MUTABLE_FORMAT_BIT` 原本一律加 `MTLTextureUsagePixelFormatView`，它會關掉 Apple 的無損壓縮（M5 Max 小程式：4 張 RGBA16F＋D32 的 load／store pass 3.1 倍；ShaderWrite 沒影響）。Metal 不帶這個旗標也能開只差 sRGB、或只改型別／層級／swizzle 的 view，所以格式清單只有自己和 sRGB 雙胞胎時不加；在算大小之前決定（壓縮要額外空間）。清單外的 view 會印 `kk: view as …`（最多 8 次）。vkd3d-proton 的清單一定含 UINT 等，要配 `vkd3d-proton/0002` 的開關才有效 |
| `0014-…resolve-timestamps-at-the-end-of-the-Metal-command-buffer` | 我們的。timestamp 的 counter heap resolve 原本在每個寫過 timestamp 的 encoder 結束後馬上做，下一個 encoder 要等它（M5 Max 沒錄 trace 時每次約 16–26 µs；trace 上是 command buffer 裡 40–160 µs、少一個 encoder 編號的空檔，飛行中每幀約 76 個，紀錄 25.57–25.59）。改成延到 Metal command buffer 結尾一起做，相鄰的照舊合併；`vkCmdCopyQueryPoolResults` 讀、`vkCmdResetQueryPool` 清 pool 之前，和換下一個 Metal command buffer 之前（16384 次上限、pre_gfx）先落地；reset 的 dispatch 另外對 Blit stage 下 barrier（copy 原本就有）。小程式：dispatch 和 draw 之間有 timestamp 54.6 → 29.0 µs、draw 後面有 timestamp 43.5 → 23.0 µs（沒 timestamp 是 22.3），照模擬器形狀的 list（timestamp、dispatch、draw、render pass 裡的 timestamp、ResolveQueryData）99.4 → 83.3 µs；copy 前那次 resolve 省不掉。`MESA_KK_EAGER_TS_RESOLVE=1` 退回逐個 encoder 做 |
| `0015-…command-pools-keep-the-BOs-their-command-buffers-use` | 我們的。command pool 原本最多留 33 個空閒的上傳 BO（每個 128 KiB）。vkd3d-proton 每個 D3D12 command list 用一個 command buffer，加一個放 query reset 的 init command buffer，各自第一筆上傳就拿一個 BO，所以一個 allocator 兩次 Reset 之間錄超過約 16 個 list，每次 `vkFreeCommandBuffers`（`ID3D12CommandAllocator::Reset`）就毀掉多的、錄下一批時再建：每次一個 Metal heap 加 buffer，進出 residency set，下一次提交的 residency commit 再處理一次。MSFS 每幀約 129 個 list（25.64），跑道頭 5 秒的 Time Profiler 扣掉錄製成本，建 BO 約 360 ms（Render Thread）、主執行緒毀 34 ms、提交執行緒釋放 heap 118 ms，紀錄 25.72。改成留到上次 `vkResetCommandPool` 以來同時在用的最多個數（下限 32、上限 1024），Reset 時放掉多的；順便補上建 pool 時沒初始化的空閒計數。小程式 `d3d12gpu cpool`：128 個 list 一個 allocator，Reset 2.43 → 0.33 ms、錄製 5.78 → 3.55 ms、程序 CPU 18.1 → 13.9 ms／幀；4 個 allocator 錄製 4.55 → 3.27 ms；16 個 allocator（各 8 個）不變；footprint 多 5–43 MB。`MESA_KK_CMD_POOL_FIXED_CAP=1` 退回舊的上限 |

Metal 只對「透過帶 `raster_order_group` 屬性的參數」做的存取保證順序；從普通位址轉型拿到的貼圖 handle 或結構成員，屬性不會生效（M5 Max 實測：N 個重疊片段各做一次讀出加一寫回，只有經過參數的才剛好等於 N）。

## sdl/

套在 SDL `release-3.4.20`（`f00dc91`，Homebrew 11/7 的版本）上，`tools/sdl-build/build-sdl.sh` 每次把原始碼樹重設回 tag 再套一次，編好的 `libSDL3.0.dylib` 放 `~/GitHub/msfs-src/sdl/stage/`，`tools/hadron/build-runtime.sh` 用 `HADRON_SDL3` 換掉 Homebrew 的 SDL3（sdl2-compat 照舊用 Homebrew 的）。

| 檔案 | 內容 |
|---|---|
| `0001-hidapi-mac-match-only-controllers…` | 我們的。SDL 預設只列舉手把（`SDL_HINT_HIDAPI_ENUMERATE_ONLY_CONTROLLERS`），但 mac 的 hidapi 原本用 NULL 比對：IOHIDManager 每次列舉都替機器上每個 HID 裝置（鍵盤、觸控板、加速度計、光感……，這台 18 個）建一個 IOHIDDevice、開一次 user client，列完才丟掉不是手把的。SDL 只要有任何 HID 裝置出現或消失就重新列舉。11/7 12:50 核心在這條路上 panic（MTE 抓到 use-after-free，`IOConnectSetCFProperties` ← `IOHIDDeviceCreate` ← `IOHIDManagerSetDeviceMatchingMultiple`，winedevice 的 SDL 執行緒）。改成一開始就只比對過濾會留下的那些：Generic Desktop 的 Joystick、GamePad、MultiAxis（任一組 usage 都算），加上 Valve、Flydigi。這台每次列舉建的裝置 18 → 0；hint 關掉、指定 VID／PID 的列舉照舊 |

## fex/

依序套在上游 FEX main `02e24ca5c`，結果等於 fork 分支 `steamac-darwin` 的 `995db7b36`。

| 檔案 | 內容 |
|---|---|
| `0001-…x86-execute-breakpoints…` | FEXCore 的 JIT 在已設斷點的位址前插檢查，命中送 #DB（DR6、EFLAGS.RF 語意）；ARM64EC 前端每執行緒保存 DR0–DR7，執行期 detour CrossOver ntdll 的 ARM64EC `NtSetContextThread`／`NtGetContextThread`（它的 x64 context 轉換會丟掉除錯暫存器） |
| `0002`–`0004` `…STEAMAC_EXCLOG…` | 只記錄：`STEAMAC_EXCLOG=1` 時記下交給模擬器的例外、模擬器的堆疊、主程式頭幾次存取違規的暫存器與可讀字串 |
| `0005-…drop-the-pending-RF…` | 例外處理把執行位置改到別處時清掉待處理的 RF，否則下一次命中同一個斷點會被吞掉 |
| `0006-…adaptive-per-page-SMC…` | SMC 熱頁：同一頁寫入故障太頻繁就不再防寫，改成從那頁編出的指令執行前逐條檢查自己的位元組（`STEAMAC_SMC_HOT=0` 關閉，`STEAMAC_SMCSTATS=1` 印統計） |
| `0007-…hardware-TSO-on-macOS…` | unixlib 可以在 macOS 編：`SetHardwareTSOControl` 呼叫 `thread_set_x86_64_compat()`（macOS 26 SDK 的公開函式，Wine 的 loader 有它要的權限），其他只有 Linux 有的控制回 `STATUS_NOT_SUPPORTED`；這個開關只管呼叫它的執行緒，所以 `ThreadInit` 每條新執行緒都再開一次。沒有這個 unixlib 時 FEX 用軟體模擬 x86 記憶體順序（`tools/tso-bench`） |

只支援執行斷點，watchpoint 沒做。x64 程式碼直接呼叫 `NtSetContextThread` 的 syscall thunk 時拿不到除錯暫存器，現在只涵蓋經過 kernel32／kernelbase `SetThreadContext` 的路徑。

編譯（Mac，llvm-mingw 20261020）：

```sh
cmake -S FEX -B build -G Ninja \
  -DCMAKE_BUILD_TYPE=Release \
  -DCMAKE_C_COMPILER=arm64ec-w64-mingw32-clang -DCMAKE_CXX_COMPILER=arm64ec-w64-mingw32-clang++ \
  -DMINGW_TRIPLE=arm64ec-w64-mingw32 -DTUNE_CPU=none \
  -DENABLE_LTO=False -DBUILD_TESTING=False -DENABLE_JEMALLOC_GLIBC_ALLOC=False
ninja -C build arm64ecfex
```

產物 `build/Bin/libarm64ecfex.dll` 就是 `xtajit64.dll`。Python 要裝 `setuptools<81`。

unixlib（runner 的 `lib/wine/aarch64-unix/libarm64ecfex.so`，取代 CrossOver 的；CrossOver 那份的呼叫編號跟上游不同）用 Xcode 的 clang 編，見 `tools/fex-unixlib/build.sh`：

```sh
xcrun clang++ -std=c++20 -O2 -arch arm64 -mmacosx-version-min=26.0 -shared -fPIC \
  -o libarm64ecfex.so FEX/Source/Windows/UnixLib/FEXUnixLib.cpp
```

AI 協助寫的，不送 FEX 上游（上游 `CONTRIBUTING.md` 不收 AI 產生的程式碼）。
