# patches

| 檔案 | 套在哪 | 內容 |
|---|---|---|
| `wine/advapi32-steamac.patch` | 上游 Wine 11.17（`36b6a2c`） | `CredEnumerateW` 把宿主回的 `ERROR_NOT_FOUND` 也當成沒有宿主憑證；`GetUserNameA/W` 照 CrossOver Hack 12735 回 `crossover` |
| `wine/msvproc-arm64ec-yuv2rgb.patch` | 上游 Wine 11.17（`36b6a2c`） | 內建 FFmpeg 的 ARM64EC `yuv2rgb_neon.S`：先收窄 B1／B2，避免後 8 個像素的紅色蓋掉前 8 個的藍色 |
| `dxvk/0001-…` | DXVK `3cb664e1`（v2.6.2-23） | D3D11 共用貼圖改走宿主記憶體，不靠 Win32 handle |
| `vkd3d-proton/0001-…` | vkd3d-proton `21a49c97`（v3.0b） | 同上，D3D12 那一側 |

編法見 `tools/wine-advapi32/build.sh`、`tools/wine-msvproc/`、`tools/d3d-build/build-d3d.sh`。

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
