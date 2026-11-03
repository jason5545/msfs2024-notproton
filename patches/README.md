# patches

| 檔案 | 套在哪 | 內容 |
|---|---|---|
| `wine/advapi32-steamac.patch` | 上游 Wine 11.17（`36b6a2c`） | `CredEnumerateW` 把宿主回的 `ERROR_NOT_FOUND` 也當成沒有宿主憑證；`GetUserNameA/W` 照 CrossOver Hack 12735 回 `crossover` |
| `wine/msvproc-arm64ec-yuv2rgb.patch` | 上游 Wine 11.17（`36b6a2c`） | 內建 FFmpeg 的 ARM64EC `yuv2rgb_neon.S`：先收窄 B1／B2，避免後 8 個像素的紅色蓋掉前 8 個的藍色 |
| `dxvk/0001-…` | DXVK `3cb664e1`（v2.6.2-23） | D3D11 共用貼圖改走宿主記憶體，不靠 Win32 handle |
| `vkd3d-proton/0001-…` | vkd3d-proton `21a49c97`（v3.0b） | 同上，D3D12 那一側 |

編法見 `tools/wine-advapi32/build.sh`、`tools/wine-msvproc/`、`tools/d3d-build/build-d3d.sh`。

## kk/

套在 Mesa `ce576c29ed` 加 fxgl/steamac v1.9.7 的 KosmicKrisp patch 0001–0077 之後：`tools/kk-build/build-kk.sh prepare patches/kk/0001-… patches/kk/0002-… patches/kk/0003-… patches/kk/0004-…`（依序）。

| 檔案 | 內容 |
|---|---|
| `0001-…fragment-shader-interlock…` | D3D12 的 ROV（vkd3d-proton 轉成 `SPV_EXT_fragment_shader_interlock`）改用 Metal 的 raster order group。用到 interlock 的 fragment shader，每種可寫貼圖型別多一個 `[[buffer(3+N)]]` 參數，device 記憶體多一個 `[[buffer(11), raster_order_group(0)]]` 參數，所有存取的位址都從這些參數推出來。並宣告 `VK_EXT_fragment_shader_interlock`。原本 `begin/end_invocation_interlock` 會印成 `Unknown intrinsic`，Metal 編譯失敗，`vkEndCommandBuffer` 回 `VK_ERROR_INVALID_SHADER_NV`，接著 `vkQueueSubmit2` 崩潰 |
| `0002-…drop-the-draw…` | 背景編譯失敗的 pipeline 只丟掉那次繪圖，不再讓 `vkEndCommandBuffer` 回錯誤（vkd3d-proton 會照樣送出這種 command buffer，在 `vkQueueSubmit2` 崩潰） |
| `0003-…keep-the-MSL…` | Metal 編譯失敗時把 MSL 和錯誤存到 `~/Library/Logs/kk-failed-msl/`（最多 200 份） |
| `0004-…PrimitiveShadingRateKHR…` | 拿掉 vertex shader 的 `PrimitiveShadingRateKHR` 輸出：dxil-spirv 看到 `SV_ShadingRate` 就會宣告，KK 沒有可變速率著色、MSL 也沒有這個輸出名稱，原本印成 `(null)` 編不過（MSFS 主選單 12 個 shader）。沒有 VRS 本來就是全速率著色 |

Metal 只對「透過帶 `raster_order_group` 屬性的參數」做的存取保證順序；從普通位址轉型拿到的貼圖 handle 或結構成員，屬性不會生效（M5 Max 實測：N 個重疊片段各做一次讀出加一寫回，只有經過參數的才剛好等於 N）。

## fex/

依序套在上游 FEX main `02e24ca5c`，結果等於 fork 分支 `steamac-darwin` 的 `c33b6a5e9`。

| 檔案 | 內容 |
|---|---|
| `0001-…x86-execute-breakpoints…` | FEXCore 的 JIT 在已設斷點的位址前插檢查，命中送 #DB（DR6、EFLAGS.RF 語意）；ARM64EC 前端每執行緒保存 DR0–DR7，執行期 detour CrossOver ntdll 的 ARM64EC `NtSetContextThread`／`NtGetContextThread`（它的 x64 context 轉換會丟掉除錯暫存器） |
| `0002`–`0004` `…STEAMAC_EXCLOG…` | 只記錄：`STEAMAC_EXCLOG=1` 時記下交給遊戲的例外、遊戲堆疊、主程式頭幾次存取違規的暫存器與可讀字串 |
| `0005-…drop-the-pending-RF…` | 例外處理把執行位置改到別處時清掉待處理的 RF，否則下一次命中同一個斷點會被吞掉 |
| `0006-…adaptive-per-page-SMC…` | SMC 熱頁：同一頁寫入故障太頻繁就不再防寫，改成從那頁編出的指令執行前逐條檢查自己的位元組（`STEAMAC_SMC_HOT=0` 關閉，`STEAMAC_SMCSTATS=1` 印統計） |

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

AI 協助寫的，不送 FEX 上游（上游 `CONTRIBUTING.md` 不收 AI 產生的程式碼）。
