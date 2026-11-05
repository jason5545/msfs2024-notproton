# Squark：MSFS 2024 在 Apple Silicon Mac 上

**正體中文** | [English](README.en.md)

讓 Microsoft Flight Simulator 2024（Steam 版）在 Apple Silicon Mac 上跑起來的修正與工具。app 是我們從 [Hadron](https://github.com/nheisterkamp/Hadron) fork 出來的 Squark（[`jason5545/squark-app`](https://github.com/jason5545/squark-app)；Hadron 是 [NotProton](https://github.com/NotProtonNot/NotProton) 的 fork，macOS 原生 Steam 的 Steam Play，自己從原始碼編 arm64 Wine runtime），arm64 引擎換成 [Highball](https://github.com/gauthierpiarrette/highball-engine) 為 MSFS 調過的那套，再加上我們自己的 D3D12 圖形層（vkd3d-proton＋DXVK 編成 ARM64EC，跑在 KosmicKrisp 上）。

全部從開放原始碼編，遊戲檔案不動。

名字是 squawk（應答機代碼）加 quark（夸克，組成 hadron 的粒子）；squark 也是超對稱理論裡夸克的夥伴粒子。這個 repo 11/3 從 `msfs2024-notproton` 改名；11/3 中午之前走的是 NotProton（CrossOver 27 Preview 的 Wine）路線，那段的 patch 和工具留在這裡當紀錄。11/4 起，fork 出來的 app 也改叫 Squark：跟上游 Hadron 差太多了，名字分開才不會混淆。

## 現況（2026/11/4）

- NotProton 路線（到 11/3 中午）：從開機走到 Xbox 自動登入、世界地圖，按「開始飛行」進到機場，駕駛艙畫面正常，停在跑道上約 16 FPS。還沒起飛。
- 換到 Hadron 之後：runtime 編好、整套回歸測試過了（Hadron 自己的 `check-runtime.sh` 8 項、Highball 的 x64 忠實度測試數字跟它的 README 一致、我們的除錯暫存器／例外／SMC／TSO／憑證／DPAPI 測試、D3D12 與 D3D11↔D3D12 共用貼圖在 KosmicKrisp 上逐像素一致），Steam 也接上了。
- 11/3 下午第一次用 Hadron 開遊戲：Xbox 自動登入，載入畫面沒有條紋，開遊戲到標題畫面約 9 分鐘。Free Flight 選 A330-200、桃園 RCTP 05R，按「開始飛行」不到兩分鐘進到駕駛艙，畫面正常、儀表在走，停在跑道上約 30 FPS（主選單約 60 FPS）。從主選單 Quit to Desktop 正常結束。整輪都是背景模式，見下面「背景模式」。
- 11/4 第一架官網版外掛：TFDi MD-11（Install Manager 是 WPF 程式）。它在 Wine 裡一勾模擬器就卡死，原因是 winemac 的主執行緒在更新 OpenGL 時排隊等它自己，上游 Wine 也還沒修，我們加了 Wine patch 0060 修掉。之後用 Wine 內建的 Wine Mono 就能裝完，不用另外裝 .NET Framework。MD-11F 在台北松山 RCSS 進得了駕駛艙，PFD、ND、EICAS、MCDU 都正常，停在跑道上約 15 FPS（同一台機器 A330 約 30 FPS）。
- 11/4 下午：fork 出來的 app 改名 Squark（app 名稱、資料夾、Steam 裡的工具名稱），GitHub 是 [`jason5545/squark-app`](https://github.com/jason5545/squark-app)。FEX patch 0035–0038（FBW A380 基準快 5.5%）裝進遊戲用的 runtime 和 prefix，MD-11 在遊戲裡的 FPS 還沒量。
- 11/4 傍晚：把 MD-11 的 WASM 拿到模擬器外面、用錄好的快照回放量 FEX。原本量到的「FEX 是原生的 3.1 倍」大半來自測試程式本身：wasm2c 編 Windows 版時每次函式呼叫都用 TLS 數呼叫深度（Mac 版沒有），MD-11 每幀約 84 萬次呼叫，這個計數器在 FEX 上變成一條串在記憶體上的相依鏈。不數深度的版本 FEX 只是原生的 1.41 倍（週期），比 A380 基準的 1.8 倍還低。MSFS 自己怎麼編 WASM 不知道（遊戲檔案不拆）。新的 FEX patch 0039 在 MD-11 回放上再省 6% 週期，16:4x 裝進遊戲用的 runtime 和 prefix，遊戲裡還沒量。
- 11/5 午休：MSFS 的 HTML／JS 儀表跑在 Coherent GT 的 JavaScriptCore 上，之前啟動選項帶 `JSC_useJIT=false`，JS 只走直譯器。開 JIT 時 FEX 偶爾會跑到 JSC 改寫前的舊翻譯而崩潰，修法是區塊失效時把入口改成交回 dispatcher（FEX patch 0040）。裝上修法、拿掉 `JSC_useJIT=false` 後，FBW A380X 停在 RCSS 28 駕駛艙從 21 FPS 變成 36–38 FPS，停了約 3 分鐘沒崩潰。一幀從 48 ms 降到 27 ms。下午驗收時發現午休那版在「另一條執行緒一直往程式碼旁邊寫資料」時會一直重新翻譯（Highball 的 `smcretire` 那段從 1 秒變 89 秒），改成「剛編好的區塊這一次直接進去」後回到原本的速度，14:1x 換進遊戲；回歸測試 25 項、`leatest`、`fault-test`、A380X 和 MD-11 的雜湊、24 個 JS 儀表的輸出都跟 0039 相同。

![TFDi MD-11F 在 RCSS 跑道頭的駕駛艙，PFD、ND、EICAS、MCDU 都有畫面](docs/img/md11-rcss-cockpit.jpg)

![FBW A380X 在 RCSS 28 跑道頭的駕駛艙，JS 開 JIT 後 37 FPS](docs/img/a380x-rcss-cockpit-jsjit.jpg)

## 環境

| 項目 | 版本 |
|---|---|
| Mac | M5 Max（40 核 GPU）、128 GB、macOS 27.2 |
| Squark（app） | [`jason5545/squark-app`](https://github.com/jason5545/squark-app) 分支 `steamac`：Hadron 上游 `87c4265` 加我們的改動，11/4 從 Hadron 改名。Squark 1.0.3，Sparkle 自動更新關掉 |
| Wine runtime | `wine-11.18-fex`：WineHQ 11.18，加 Hadron 的 patch 0001–0003、0005，Highball `patches-arm64` 0018–0052（highball-engine `2eedc50`），我們的 0004、0053–0060；runtime 帶 Homebrew 的 sdl2-compat＋SDL3，winebus 走 SDL（照 Highball） |
| FEX | FEX-2610 加 Highball `fex/patches` 0001–0033，unix 端是 Highball 的 `fexunixlib_darwin.cpp`（每條執行緒都開硬體 TSO）。我們的 0035–0038（以 FBW A380 基準最佳化，說明在 fork 的 `runtime/fex/highball/README.md`）11/4 裝進遊戲用的 runtime 和遊戲 prefix；0037 由 MSFS 配方對 `\wasm\msfs2024\` 開；0039（入口不再存區塊 header，MD-11 回放 −6% 週期）11/4 傍晚也裝進去了；0040（區塊失效時入口改成交回 dispatcher，修掉 JSC 開 JIT 時偶爾跑到改寫前的翻譯而崩潰，`SQUARK_FEX_STALE_EXIT=0` 關掉）11/5 裝進去 |
| 圖形 | KosmicKrisp（Mesa `ce576c29ed` 加 fxgl/steamac v1.9.7 的 0001–0077，再加 [`patches/kk/`](patches/kk/)）；vkd3d-proton `21a49c97`（v3.0b）、DXVK `3cb664e1`（v2.6.2-23）加 [`patches/vkd3d-proton/`](patches/vkd3d-proton/)、[`patches/dxvk/`](patches/dxvk/)，編成 ARM64EC |
| 其他 | GStreamer 1.28.7（官方 framework，用 Highball 的腳本精簡；沒有它開始畫面會一直黑） |
| MSFS 2024 | Steam AppID 2537590，1.9.21.0 |

Wine runtime 的 patch、編法和 Hadron 本身的改動都在 fork 裡（`runtime/patches/wine-11.18-fex/`、`runtime/fex/highball/`、`runtime/gstreamer/`、`runtime/build-wine.sh`、`dylib/feats/compat_run.sh`）。Highball 的檔案原樣搬過去、不改原檔，要修就加新編號的 patch。這個 repo 的 `tools/hadron/build-runtime.sh` 呼叫 fork 的 `build-wine.sh`，把 KosmicKrisp 和 ARM64EC 的 vkd3d-proton、DXVK 一起帶進 runtime。

Wine 的 loader 是 runtime 裡的 `Wine.app`，要有 Cross-architecture Compatibility Framework 權限才能在 arm64 上跑 x86 程式碼：Apple Developer 網站上建一個勾了這項的 App ID，`xcodebuild -allowProvisioningUpdates` 就會產生描述檔（Xcode 不會替這項權限自己註冊 App ID）。

## 我們的 Wine patch（fork 的 `runtime/patches/wine-11.18-fex/`）

| patch | 內容 |
|---|---|
| 0004 | aarch64 loader 的連結參數（PAGEZERO、image base、x86_64 版面）和從 `Wine.app` 找 `ntdll.so`。Hadron 原本的 0004 其餘部分 Highball 都有，而且更完整 |
| 0053 | Highball 0030 的 `dlopen_host_lib` 加轉型，讓 `wine/unixlib.h` 也能當 C++ 編（lsteamclient 的 unix 端） |
| 0054 | `HADRON_WINE_USER` 指定 Windows 使用者名稱。Wine 的 DPAPI 把使用者名稱混進金鑰，CrossOver 時代以 `crossover` 加密的資料（Xbox 登入）要同一個名字才解得開；NotProton 留下的 prefix 由 `compat_run.sh` 自動設 |
| 0055 | `HADRON_VK_HOST_SHARING=1` 不給程式 `VK_KHR_external_memory_win32`。Highball 0036 透過 Metal 物件提供它，我們的 DXVK／vkd3d-proton 看到它就不走驗證過的宿主記憶體共用，D3D11↔D3D12 共用貼圖會失敗；選我們的圖形層時自動設 |
| 0056 | `HADRON_MAC_BACKGROUND=1` 背景模式，給自動化測試用：視窗放在桌面圖層底下（畫面上看不到，但照常畫、截得到）、永遠不搶前景、不動主機游標和解析度、不經 macOS 輸入法，輸入只從 Wine 內部送 |
| 0057 | crypt32 `PFXImportCertStore` 只把私鑰掛給公鑰相符的那張憑證。Wine 原本掛給 PFX 裡每一張，.NET 刪掉不要的憑證時連共用的金鑰容器一起刪，留下的伺服器憑證就沒有私鑰（MSFS Map Enhancement 的本機 HTTPS 伺服器踩到）。上游還沒修 |
| 0058 | schannel 找伺服器憑證的 CAPI 私鑰時，HKCU 找不到再找 HKLM（.NET 的 `MachineKeySet`） |
| 0059 | `SQUARK_FAKEPAD=<FIFO 路徑>`（或 prefix 裡有 `squark-fakepad` 這個 FIFO）時，winebus 的 SDL 匯流排多掛一支模擬手把（045e:02fd），從 FIFO 讀文字指令。沒有實體手把時測試輸入、背景操作用 |
| 0060 | winemac 的 `OnMainThread` 在主執行緒上就直接執行。主執行緒在 `viewWillDraw` 更新 OpenGL context 時排隊等自己，整個程式卡死（TFDi MD-11 的 Install Manager，WPF 走 d3d9→wined3d OpenGL）。上游 master 還是一樣 |

## 圖形與啟動選項

遊戲的 Squark 面板「圖形」選「vkd3d-proton + KosmicKrisp」（`HADRON_GRAPHICS=vkd3d`）：覆蓋層 `lib/graphics/vkd3d`＋`lib/graphics/dxvk` 排在 Wine 自己的 DLL 前面，Vulkan 走 KosmicKrisp，並設 `VKD3D_FEATURE_LEVEL=12_1`、`VKD3D_SHADER_MODEL=6_7`、`DXVK_CONFIG`。只有 arm64（FEX）runtime 有這個選項。MSFS 現在的 Steam 啟動選項還是 NotProton 時期的寫法，`compat_run.sh` 會對應成 `HADRON_*` 的名稱（改名成 Squark 之後，這些變數名稱照舊）：

```
CX_GRAPHICS_BACKEND=vkd3d %command%
```

11/5 以前還帶 `NOTPROTON_ENV_JSC_useJIT=false`，讓 Coherent GT 的 JavaScriptCore 只走直譯器，因為開 JIT 時 FEX 會偶爾崩潰。FEX patch 0040 修掉之後拿掉了，A380X 從 21 FPS 變成 36–38 FPS（見上面的現況）。

`CX_GRAPHICS_BACKEND` 對應 `HADRON_GRAPHICS`，`NOTPROTON_ENV_<名稱>` 對應 `HADRON_ENV_<名稱>`（已經有 Hadron 名稱的優先）。MSFS（2537590）在 arm64 runtime 上自動帶 Highball 的 MSFS SMC 設定（`HB_FEX_SMC_RETIRE=1 HB_FEX_SMC_SKIP_SAME=1 HB_FEX_SMC_HOT_MAXINST=64 HB_FEX_VM_FASTPATH=1 HB_FEX_SMC_DEFER_CALLRET=1`），以及 `SQUARK_FEX_ABI_FLAGS=\wasm\msfs2024\`（FEX 0037，只作用在遊戲為飛機和儀表編出來的 WebAssembly 模組），啟動選項有設的優先。

DXVK 和 vkd3d-proton 跟遊戲在同一個行程裡。編成 ARM64EC 之後遊戲照樣當 x64 DLL 載入，裡面的程式碼直接跑原生 ARM64，D3D 轉 Vulkan 這一步不用再跟遊戲一起在 FEX 裡模擬。離線測試（`tools/d3d-bench/`）：D3D12 錄每次 draw 的時間少約 7–16%，第一次建 PSO 快很多（8 條執行緒同時建 96 個：x64 0.6–1 秒、ARM64EC 0.03 秒）；代價是每次呼叫都要跨一次 x64／ARM64EC 的邊界，D3D11 這種一次只做一點事的呼叫反而慢（約 50 → 190 ns）。11/3 遊戲在 Hadron 上用的就是 ARM64EC 這組，主選單和駕駛艙都畫得正常；跟 x64 版在遊戲裡的 FPS 還沒比過。

## 背景模式（自動化測試用）

開遊戲測試時，遊戲畫面不能出現在使用者的螢幕上，也不能搶走鍵盤焦點，因為使用者同時在用這台 Mac。做法分三層：

- **Wine（0056）**：`HADRON_MAC_BACKGROUND=1` 時，視窗一建立就放在最低的視窗層（桌面圖層底下），還在螢幕上，所以照常畫、`screencapture -l` 截得到；程式永遠不自己啟用、不取消隱藏，不動、不鎖主機游標，不改解析度，按鍵不經 macOS 輸入法。
- **Squark `compat_run.sh`**：啟動選項 `HADRON_MAC_BACKGROUND=1`，或一次性請求檔 `~/Library/Application Support/squark/background/<AppID>`（10 分鐘內有效）。設好之後才跑任何 Wine 程序（prefix 更新的 wineboot 也藏住），`open -g` 啟動、不開 Squark 的啟動視窗。Steam 開遊戲之前會先跑一次安裝腳本（`iscriptevaluator.exe`，verb `run`），那次也走背景，但請求留給遊戲本身那次（`waitforexitandrun`）拿；11/3 第一次試的時候請求被安裝腳本先用掉，遊戲變成正常模式出現在畫面上（fork `56dcd8b` 修掉）。
- **輸入**：從 Wine 內部送硬體輸入（一支小的 Windows 程式加入遊戲的 wineserver，透過 win32u 的 `SendHardwareInput` 送，跟 winemac 轉送 Mac 輸入走同一條路），不動 Mac 的游標和前景。MSFS 的按鈕要先 hover，移動兩次再點就按得到；搜尋框打字也收得到。監看程式每 30 ms 檢查遊戲和 Steam 的視窗，有視窗跑到桌面之上就立刻藏起來、搶走前景就還回去。這部分工具只放本機，不在 repo 裡。

11/3 一整輪（開遊戲、登入、主選單、Free Flight、進駕駛艙、退出）遊戲視窗 0 次出現在畫面上、0 次搶前景。

## 修掉的問題

下面 1–5 是 NotProton 路線上查到、修好的。換到 Hadron 後，每一項怎麼處理寫在各段最後。11/3 在 Hadron 上開遊戲，1、2、3、5 都沒再出現（Xbox 自動登入、沒有條紋、過了 8/22、按開始飛行進到駕駛艙）。

### 1. Xbox 每次重開都要重新登入

- XAL 只用 `CredEnumerateW(filter*)` 讀存好的憑證，包括 DeviceKey。
- Wine 的 `CredEnumerateW` 會把登錄檔裡的憑證，跟 mountmgr 從宿主鑰匙圈查到的合在一起。macOS 版的 `search_credentials` 找不到符合項目時回 `STATUS_NOT_FOUND`，到 advapi32 變成 `ERROR_NOT_FOUND`（1168）；advapi32 只把 `ERROR_NOT_SUPPORTED` 當成「宿主沒有」，所以整個列舉失敗。Linux 回的是 `NOT_SUPPORTED`，不會出事。
- 結果 XAL 每次都以為沒有裝置金鑰，重新產生一把，登入也就跟著要重來。
- 修法：`ERROR_NOT_FOUND` 也當成宿主沒有憑證（`patches/wine/advapi32-steamac.patch`）。
- 驗證（NotProton）：DeviceKey 的雜湊重開後不變，權杖靜默更新，沒有再跳登入畫面。`tools/cred-test/` 測 CredWrite／Read／Enumerate、沒有符合項目的情況、BCrypt ECDSA。
- **Hadron 路線**：Highball 的 Wine 0034 是同一個修法。另外 Wine 的 DPAPI 把使用者名稱混進金鑰，CrossOver 底下是 `crossover`、Hadron 是 `steamuser`，名字一換，舊的登入資料就解不開（`tools/cred-test/dpapitest.c` 實測），所以加了 0054，NotProton 留下的 prefix 照舊用 `crossover`。

### 2. 載入畫面背景整片直條紋、偏黃藍

- Wine 的 `msvproc.dll` 內建 FFmpeg，它的 ARM64EC `yuv2rgb_neon.S` 在 NV12→BGRA 時有暫存器衝突：每 16 個像素，前 8 個的藍色被後 8 個的紅色蓋掉。
- CrossOver 27 原本那顆，跟上游 Wine 11.17 原樣自己編出來的，輸出一模一樣，所以這是上游原始碼的問題。
- 修正後離線比對，藍色誤差從 67.75 降到 0.58；遊戲裡直條紋消失，同一塊天空的條紋強度從 37.48 降到 0.27。`tools/share-test/` 的 `mfgrab` 用來離線重現。
- **Hadron 路線**：Highball 的 Wine 0037 修的是同一個檔案、同一個錯。

### 3. 登入時卡在「Authentication in progress (8/22)」

- 遊戲每格都用 x86 硬體除錯暫存器設執行斷點，FEX ARM64EC 原本不支援。
- 補上之後還有一個問題：例外處理（VEH）改了 RIP 之後，FEX 沒清掉 RF 記號，下一次命中同一組斷點時被吞掉，接著存取違規。
- 修正在 `patches/fex/` 的 0001（執行斷點）和 0005（RF）。`tools/dr-test/`、`tools/eh-test/` 是對應的測試程式。
- **Hadron 路線**：Highball 的 FEX 0017 和 Wine 0029。它照真的 Windows 的語意做：handler 拿到的 context 沒有 RF，不改 context 就繼續的話，同一個斷點會一直觸發，直到 handler 自己關掉（Highball 在 Windows Server 上驗過）。我們的 `dr-test`、`eh-test` 是照 Linux 上 Wine 的行為寫的期待，在這套 FEX 上斷點那段會逾時，測試期待還沒改。

### 4. 載入時「四槽斷點」那段很慢

- 載入中有一段會反覆命中四個斷點約 700 輪，每輪都牽涉自我修改程式碼（SMC）。ARM64EC 的 FEX 每次 SMC 都要走一次頁故障，一輪約 0.8 秒，整段要 9 分鐘左右。
- 把在 Linux 版 FEX 做過的「熱頁」（同一頁故障太多次就改成逐指令檢查）搬到 ARM64EC 之後，698 輪 85.8 秒跑完（`patches/fex/` 的 0006）。`tools/smc-test/` 量單次 SMC 的成本。
- 同一段在 NotProton 上還少了硬體 TSO（`patches/fex/` 的 0007），`tools/tso-bench/` 量得到差別。
- **Hadron 路線**：Highball 的 FEX 有自己的 SMC 對策（0020、0023 等），MSFS 自動帶它的 MSFS 設定，比全關快 2–5 倍、全部正確（`tools/smc-msfs/`）；硬體 TSO 每條執行緒都開。資料和程式碼在同一頁、一直寫的情況，它仍比我們舊的熱頁慢（每輪約 2 萬 ns 對 17 ns）；Highball 的 `HB_FEX_SMC_INLINE=1` 快得多，但跨執行緒逐步改碼的測試會卡住，沒有開。

### 5. 按「開始飛行」後卡死

- MSFS 用 D3D12 的 ROV（rasterizer ordered views）。遊戲要 Feature Level 12_1，所以 `VKD3D_FEATURE_LEVEL=12_1` 讓 vkd3d-proton 回報支援，照樣產生 fragment shader interlock 的 SPIR-V。KosmicKrisp 不認得 `begin/end_invocation_interlock`，印成 `Unknown intrinsic`，Metal 編不過；`vkEndCommandBuffer` 回 `VK_ERROR_INVALID_SHADER_NV`，vkd3d-proton 照樣送出，接著在 `vkQueueSubmit2` 崩潰。主選單時就有 270 個這種 shader，按開始飛行才真的畫到。Mesa 上游的 KosmicKrisp 也還沒有。
- Metal 對應的功能是 raster order group，但實測（M5 Max，N 個重疊片段各做一次讀出、加一、寫回）只有「從帶 `raster_order_group` 屬性的參數推出來」的存取才有順序保證；KosmicKrisp 從普通位址轉型拿到的貼圖 handle 不算。
- 修法（`patches/kk/` 的 0001）：用到 interlock 的 fragment shader 多帶幾個有屬性的參數，可寫貼圖和 device 記憶體的存取位址都從這些參數推出來，並宣告 `VK_EXT_fragment_shader_interlock`。0002 讓編不過的 pipeline 只丟掉那次繪圖、不讓整個 command buffer 失敗，0003 把 Metal 拒絕的 MSL 存下來，0004 拿掉 KosmicKrisp 沒有對應的 `PrimitiveShadingRateKHR` 輸出（主選單另外 12 個 vertex shader），0005 讓除錯輸出預設關閉。0006–0011 是 Highball 的 KosmicKrisp patch（MSL 共用省記憶體、sparse 修正、較輕的 encoder barrier），見 `patches/README.md`。
- 驗證：原生 Vulkan 測試 N=1024 時有 interlock 每個像素剛好 1024，沒有的對照組會掉數。遊戲裡 `Unknown intrinsic` 從 270 次變 0 次，按開始飛行後進到機場，駕駛艙畫面正常。
- **Hadron 路線**：同一份 KosmicKrisp 帶進 runtime（`lib/libvulkan_kosmickrisp.dylib`）。

## NotProton 路線（11/3 中午之前，停用）

這幾樣只在 NotProton 路線用，留著當紀錄：

- [`patches/notproton/`](patches/notproton/)、`tools/np-build/`：把元件編進自己的 NotProton（fork `jason5545/NotProton-steamac` 分支 `steamac`），由它寫進 CrossOver 的 FEX runner。
- [`patches/fex/`](patches/fex/)：FEX 的七個 patch（執行斷點、例外紀錄、RF、SMC 熱頁、硬體 TSO），依序套在上游 main `02e24ca5c`，等於 fork 分支 `steamac-darwin`。0001 在執行期 detour CrossOver 閉源 ntdll 的 ARM64EC `NtSetContextThread`／`NtGetContextThread`，是在「不碰閉源二進位」原則定下之前做的，之後不再用這種做法。這些 patch 是 AI 協助寫的，FEX 上游的 `CONTRIBUTING.md` 寫明不收 AI 產生的程式碼，所以只放在這裡，不送上游。
- [`patches/wine/`](patches/wine/)、`tools/wine-advapi32/`、`tools/wine-msvproc/`：從上游 Wine 11.17 編 advapi32、msvproc，換掉 CrossOver 的。
- `tools/fex-unixlib/`：NotProton 的 FEX 用的 macOS unixlib（硬體 TSO）。

## 工具

| 路徑 | 用途 |
|---|---|
| `tools/hadron/` | `build-runtime.sh` 用 fork 編 runtime；`run.sh` 在 runtime 上、一次性 prefix 裡跑一支 Windows 測試程式；`suite.sh` 跑整套回歸（我們的測試加 Highball 的 x64 忠實度測試） |
| `tools/kk-build/` | 照 fxgl/steamac v1.9.7 的方式編 KosmicKrisp |
| `tools/d3d-build/` | 編 DXVK／vkd3d-proton（x64 或 ARM64EC） |
| `tools/d3d-bench/` | D3D11／D3D12 轉 Vulkan 的 CPU 成本（x64 與 ARM64EC 比較），`ectest/` 是 ARM64EC DLL 的 C++ 例外與回呼測試 |
| `tools/cred-test/` | Credential Manager、BCrypt、DPAPI（`dpapitest.c`）測試 |
| `tools/dr-test/` | `SetThreadContext` 除錯暫存器測試 |
| `tools/eh-test/` | ARM64EC 例外處理與斷點恢復測試 |
| `tools/fault-test/` | JIT 程式碼裡的存取違規要回報正確的 RIP（入口區塊、call 回來的區塊、三層深、熱迴圈、編過幾千個區塊之後），加跨快取線的未對齊 LOCK 指令；驗 FEX 0039 |
| `tools/smc-test/`、`tools/smc-msfs/` | 自我修改程式碼的成本與正確性；`smc-msfs` 模擬 MSFS 那幾種寫法 |
| `tools/smc-subpage/` | 改寫已經翻譯過的程式碼旁邊的碼：16 KB 主機頁裡的其他 4 KB 頁、跨執行緒同時寫資料；S9（被呼叫的函式改寫 call 回去的那條指令）在 FEX 0040 之前約三成拿到舊值 |
| `tools/wine-decommit/` | Wine 的 `MEM_DECOMMIT` 再 `MEM_COMMIT` 是否把不對齊 16 KB 的頁清成零（FEX 清 call-ret 堆疊靠這個） |
| `tools/tso-bench/` | x86 記憶體順序（軟體與硬體 TSO）的正確性與成本 |
| `tools/share-test/` | D3D11↔D3D12 共用貼圖、Media Foundation 影片解碼與 `msvproc` 輸出 |
| `tools/np-gfx/` | Vulkan 與 D3D12 基本測試 |
| `tools/tls-server-test/` | Wine 裡的 .NET HTTPS 伺服器：自產憑證或載入 PFX（各種金鑰儲存方式）當 TLS 伺服器，重現 0057、0058 |
| `tools/clrstack/` | 用 ClrMD 印出執行中 .NET Framework 程式每條執行緒的堆疊（在 Wine 裡跟目標同一個 runner、prefix 跑），找到 0060 的死結 |
| `tools/padprobe/` | 手把：`padprobe` 在 Wine 裡讀 XInput，`macpad`、`sdlpad` 在 Mac 端讀實體手把，`fakepad.py` 驅動 0059 的模擬手把 |
| `tools/wasm-bench/` | WASM 航電基準：C 或 Rust → wasm32 → wasm2c → x64（跟 MSFS 把 WASM 編成 x64 dll 同一型），另編 Mac 原生當對照。`fbwbench` 是 FlyByWire A380X 的 Rust 系統模擬（GPL-3.0，原始碼另外 clone），`bigbench` 量大量程式碼，`fexstats.py` 讀 FEX 的執行緒統計。量 FEX 改動用 `bench2.sh`（暖機期差分，算每 tick 的指令數與週期，`proc_pid_rusage`）、`bench-time.sh`；看翻譯出來的程式碼用 `jitdump.sh`（lldb 只接測試程式）→ `jitimg.py` → `jitcat.py`／`jitann.py`；遊戲的 `sample` 用 `gameprof.py` 分模組；`leatest/` 是 LEA 正確性測試；`fexexp-build.sh`、`fexexp-swap.sh` 編 FEX、把 DLL 換進測試 runtime 和測試 prefix 的 system32 |
| `tools/hb-fex/` | NotProton 時期（停用）：把 FEX DLL 換進測試用 runner 複本、跑回歸測試 |

Wine 的 DLL 和測試程式用 llvm-mingw（含 arm64ec）編。

## 原則

- 只補環境，不改、不打補丁、不反組譯遊戲檔案。
- 閉源的部分（D3DMetal、Metal 驅動、Rosetta、Steam、遊戲，以及 NotProton 時期的 CrossOver Preview）只看公開介面和行為，不反組譯。要知道某個元件怎麼做，讀對應的開放原始碼。

## 授權

`patches/` 底下的 patch 沿用各自上游的授權：Wine 是 LGPL 2.1 以上，DXVK 是 zlib，vkd3d-proton 是 LGPL 2.1，Mesa（KosmicKrisp）是 MIT，FEX 是 MIT。
