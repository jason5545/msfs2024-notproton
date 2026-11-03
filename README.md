# MSFS 2024 在 Apple Silicon Mac 上：NotProton 路線

**正體中文** | [English](README.en.md)

讓 Microsoft Flight Simulator 2024（Steam 版）在 Apple Silicon Mac 上跑起來的修正與工具。用的是 [NotProton](https://github.com/NotProtonNot/NotProton)（macOS 原生 Steam 的 Steam Play，底下是 CrossOver 27 Preview 的 arm64 Wine 加 FEX ARM64EC）。

做法是把幾個開放原始碼元件從原始碼自己編，透過 NotProton 的 runner variant 疊進 runner 複本。CrossOver 的原檔不改，遊戲檔案也不動。

## 現況（2026/11/3）

- 從開機一路走完：開場影片、載入、按 Start 後 Xbox 自動登入（不用每次重新登入）、進到世界地圖（自由飛行選單）。
- 按「開始飛行」後載入完成，進到機場：駕駛艙畫面正常，停在跑道上，約 16 FPS（11/3）。
- 還沒實際起飛。

## 環境

| 項目 | 版本 |
|---|---|
| Mac | M5 Max（40 核 GPU）、128 GB、macOS 27.2 |
| NotProton | NotProton.app 1.2.1，runner `crossover-27.0.0.41073-fex`（CrossOver Preview 20261015，Wine `wine-11.17-8910-g4683e69e153`，FEX `FEX-2604-1037-gf0b5147`） |
| MSFS 2024 | Steam AppID 2537590，1.9.21.0 |

## 疊進 runner 的元件

設定在 [`config/notproton-variants-steamac.conf`](config/notproton-variants-steamac.conf)。Steam 啟動選項：

```
NOTPROTON_STEAMAC=1 NOTPROTON_RUNNER_VARIANT=steamacfex+steamackk+steamacadv+steamacvproc NOTPROTON_ENV_JSC_useJIT=false %command%
```

| variant | 換掉什麼 | 來源 |
|---|---|---|
| `steamacfex` | `xtajit64.dll`（FEX ARM64EC） | 上游 FEX `02e24ca5c` 加 `patches/fex/` 六個 patch（x86 執行斷點、例外紀錄、RF 修正、SMC 熱頁） |
| `steamackk` | Vulkan loader 1.4.363，接 KosmicKrisp | Mesa `ce576c29` 加 fxgl/steamac v1.9.7 的 KosmicKrisp patch，再加 `patches/kk/` 四個 patch（ROV 等，見問題 5），`tools/kk-build/` |
| `steamacadv` | `advapi32.dll` | 上游 Wine 11.17 加 `patches/wine/advapi32-steamac.patch`，`tools/wine-advapi32/` |
| `steamacvproc` | `msvproc.dll` | 上游 Wine 11.17 加 `patches/wine/msvproc-arm64ec-yuv2rgb.patch`，`tools/wine-msvproc/` |

D3D 這層靠 NotProton 的外掛（`NOTPROTON_STEAMAC=1`，設定在 [`config/notproton-plugin-steamac.conf`](config/notproton-plugin-steamac.conf)）：每次啟動把 DXVK 和 vkd3d-proton 的 DLL 放進 prefix 的 `system32`，Vulkan 指到 KosmicKrisp，並設 `VKD3D_FEATURE_LEVEL=12_1`、`VKD3D_SHADER_MODEL=6_7`。目前用的是 DXVK `3cb664e1`（v2.6.2-23）和 vkd3d-proton `21a49c97`（v3.0b）各加一個 patch，讓 D3D11 和 D3D12 之間透過宿主記憶體共用貼圖（`patches/dxvk/`、`patches/vkd3d-proton/`），用 `tools/d3d-build/` 編和換。

## 修掉的問題

### 1. Xbox 每次重開都要重新登入

- XAL 只用 `CredEnumerateW(filter*)` 讀存好的憑證，包括 DeviceKey。
- Wine 的 `CredEnumerateW` 會把登錄檔裡的憑證，跟 mountmgr 從宿主鑰匙圈查到的合在一起。macOS 版的 `search_credentials` 找不到符合項目時回 `STATUS_NOT_FOUND`，到 advapi32 變成 `ERROR_NOT_FOUND`（1168）；advapi32 只把 `ERROR_NOT_SUPPORTED` 當成「宿主沒有」，所以整個列舉失敗。Linux 回的是 `NOT_SUPPORTED`，不會出事。
- 結果 XAL 每次都以為沒有裝置金鑰，重新產生一把，登入也就跟著要重來。
- 修法：`ERROR_NOT_FOUND` 也當成宿主沒有憑證。另外照 CrossOver 原本的行為補上 `GetUserNameA/W` 回 `crossover`（CrossOver Hack 12735；設 `CX_REPORT_REAL_USERNAME` 就回真名）。
- 驗證：DeviceKey 的雜湊重開後不變，權杖靜默更新，沒有再跳登入畫面。`tools/cred-test/` 在 runner 裡測 CredWrite／Read／Enumerate、沒有符合項目的情況、BCrypt ECDSA。

### 2. 載入畫面背景整片直條紋、偏黃藍

- Wine 的 `msvproc.dll` 內建 FFmpeg，它的 ARM64EC `yuv2rgb_neon.S` 在 NV12→BGRA 時有暫存器衝突：每 16 個像素，前 8 個的藍色被後 8 個的紅色蓋掉。
- CrossOver 27 原本那顆，跟上游 Wine 11.17 原樣自己編出來的，輸出一模一樣，所以這是上游原始碼的問題。
- 修正後離線比對，藍色誤差從 67.75 降到 0.58；遊戲裡直條紋消失，同一塊天空的條紋強度從 37.48 降到 0.27。`tools/share-test/` 的 `mfgrab` 用來離線重現。

### 3. 登入時卡在「Authentication in progress (8/22)」

- 遊戲每格都用 x86 硬體除錯暫存器設執行斷點，FEX ARM64EC 原本不支援，要自己補。
- 補上之後還有一個問題：例外處理（VEH）改了 RIP 之後，FEX 沒清掉 RF 記號，下一次命中同一組斷點時被吞掉，接著存取違規。
- 修正在 `patches/fex/` 的 0001（執行斷點）和 0005（RF）。`tools/dr-test/`、`tools/eh-test/` 是對應的測試程式。

### 4. 載入時「四槽斷點」那段很慢

- 載入中有一段會反覆命中四個斷點約 700 輪，每輪都牽涉自我修改程式碼（SMC）。ARM64EC 的 FEX 每次 SMC 都要走一次頁故障，一輪約 0.8 秒，整段要 9 分鐘左右。
- 把在 Linux 版 FEX 做過的「熱頁」（同一頁故障太多次就改成逐指令檢查）搬到 ARM64EC 之後，698 輪 85.8 秒跑完（`patches/fex/` 的 0006）。`tools/smc-test/` 量單次 SMC 的成本。

### 5. 按「開始飛行」後卡死

- MSFS 用 D3D12 的 ROV（rasterizer ordered views）。遊戲要 Feature Level 12_1，所以 `VKD3D_FEATURE_LEVEL=12_1` 讓 vkd3d-proton 回報支援，照樣產生 fragment shader interlock 的 SPIR-V。KosmicKrisp 不認得 `begin/end_invocation_interlock`，印成 `Unknown intrinsic`，Metal 編不過；`vkEndCommandBuffer` 回 `VK_ERROR_INVALID_SHADER_NV`，vkd3d-proton 照樣送出，接著在 `vkQueueSubmit2` 崩潰。主選單時就有 270 個這種 shader，按開始飛行才真的畫到。Mesa 上游的 KosmicKrisp 也還沒有。
- Metal 對應的功能是 raster order group，但實測（M5 Max，N 個重疊片段各做一次讀出、加一、寫回）只有「從帶 `raster_order_group` 屬性的參數推出來」的存取才有順序保證；KosmicKrisp 從普通位址轉型拿到的貼圖 handle 不算。
- 修法（`patches/kk/` 的 0001）：用到 interlock 的 fragment shader 多帶幾個有屬性的參數，可寫貼圖和 device 記憶體的存取位址都從這些參數推出來，並宣告 `VK_EXT_fragment_shader_interlock`。0002 讓編不過的 pipeline 只丟掉那次繪圖、不讓整個 command buffer 失敗，0003 把 Metal 拒絕的 MSL 存下來，0004 拿掉 KosmicKrisp 沒有對應的 `PrimitiveShadingRateKHR` 輸出（主選單另外 12 個 vertex shader）。
- 驗證：原生 Vulkan 測試 N=1024 時有 interlock 每個像素剛好 1024，沒有的對照組會掉數。遊戲裡 `Unknown intrinsic` 從 270 次變 0 次，按開始飛行後進到機場，駕駛艙畫面正常。

## FEX 的修改

`patches/fex/` 是 FEX 的六個 patch，依序套在上游 main `02e24ca5c`，結果等於 fork 分支 `steamac-darwin` 的 `c33b6a5e9`（`git apply` 驗過，檔案樹相同）。

- 0001：x86 執行斷點。CrossOver 27 Preview 的 ntdll 是閉源的，它把 x64 context 轉成 ARM64 時會丟掉除錯暫存器，所以這個 patch 在執行期 detour ntdll 的 ARM64EC `NtSetContextThread`／`NtGetContextThread`，DR0–DR7 改存在 FEX 的執行緒狀態。這是在「不碰閉源二進位」原則定下之前做的，保留沿用，之後不再擴大這種做法。
- 0002–0004：`STEAMAC_EXCLOG=1` 時記錄交給遊戲的例外，只用來除錯。
- 0005：例外處理改了 RIP 之後清掉 RF 記號。
- 0006：SMC 熱頁。

這些 patch 是 AI 協助寫的。FEX 上游的 `CONTRIBUTING.md` 寫明不收 AI 產生的程式碼，所以只放在這裡，不送上游。編法見 `patches/README.md`。

## 工具

| 路徑 | 用途 |
|---|---|
| `tools/wine-advapi32/` | 從上游 Wine 11.17 原始碼編 ARM64X `advapi32.dll` |
| `tools/wine-msvproc/` | 編 ARM64X `msvproc.dll`，`variant.sh` 比對 sha256 後裝進 NotProton variant |
| `tools/kk-build/` | 照 fxgl/steamac v1.9.7 的方式給 NotProton 編 KosmicKrisp |
| `tools/d3d-build/` | 編 DXVK／vkd3d-proton，`swap-np-d3d.sh` 備份、換檔、還原 |
| `tools/cred-test/` | Credential Manager 與 BCrypt 測試 |
| `tools/dr-test/` | `SetThreadContext` 除錯暫存器測試 |
| `tools/eh-test/` | ARM64EC 例外處理與斷點恢復測試 |
| `tools/smc-test/` | 自我修改程式碼的成本與正確性（含跨執行緒寫入、改保護） |
| `tools/share-test/` | D3D11↔D3D12 共用貼圖、Media Foundation 影片解碼與 `msvproc` 輸出 |
| `tools/np-gfx/` | Vulkan 與 D3D12 基本測試 |

Wine 的 DLL 用 llvm-mingw（含 arm64ec）編，Wine configure 加 `--enable-archs=arm64ec,aarch64`，bison 要用 Homebrew 的新版。

## 原則

- 只補環境，不改、不打補丁、不反組譯遊戲檔案。
- 閉源的部分（CrossOver Preview 的 Wine 與 FEX、D3DMetal、Metal 驅動、Steam、遊戲）只看公開介面和行為，不反組譯。要知道某個元件怎麼做，讀對應的開放原始碼。

## 授權

`patches/` 底下的 patch 沿用各自上游的授權：Wine 是 LGPL 2.1 以上，DXVK 是 zlib，vkd3d-proton 是 LGPL 2.1。
