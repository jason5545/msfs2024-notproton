# patches

| 檔案 | 套在哪 | 內容 |
|---|---|---|
| `wine/advapi32-steamac.patch` | 上游 Wine 11.17（`36b6a2c`） | `CredEnumerateW` 把宿主回的 `ERROR_NOT_FOUND` 也當成沒有宿主憑證；`GetUserNameA/W` 照 CrossOver Hack 12735 回 `crossover` |
| `wine/msvproc-arm64ec-yuv2rgb.patch` | 上游 Wine 11.17（`36b6a2c`） | 內建 FFmpeg 的 ARM64EC `yuv2rgb_neon.S`：先收窄 B1／B2，避免後 8 個像素的紅色蓋掉前 8 個的藍色 |
| `dxvk/0001-…` | DXVK `3cb664e1`（v2.6.2-23） | D3D11 共用貼圖改走宿主記憶體，不靠 Win32 handle |
| `vkd3d-proton/0001-…` | vkd3d-proton `21a49c97`（v3.0b） | 同上，D3D12 那一側 |

編法見 `tools/wine-advapi32/build.sh`、`tools/wine-msvproc/`、`tools/d3d-build/build-d3d.sh`。
