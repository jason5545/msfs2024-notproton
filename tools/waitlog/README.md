# waitlog

讀 Wine patch 0061（squark-app `runtime/patches/wine-11.18-fex/0061-ntdll-wait-recorder.patch`，等待紀錄）錄下的檔案：每條執行緒忙多少、等多少、等在哪條路、被誰叫醒（紀錄 25.54）。

## 怎麼錄

1. 程式（例如模擬器）啟動**之前**，prefix 裡要有 `squark-waitlog/` 資料夾。有這個資料夾，ntdll 才會在那個行程開一條監看執行緒；沒有就完全不動。
2. 程式在跑的時候，在那個資料夾寫一個 `ctl`（先寫 `ctl.tmp` 再改名）：

   ```
   id=<每次不同>
   exe=FlightSimulator2024.exe   # 行程的檔名，不分大小寫；* 是全部
   seconds=10                    # 窗口長度
   warmup=0.5                    # 先錄這麼久才開始算（窗口開始前就在等的那筆錄不到）
   events=2000000                # 緩衝區筆數，一筆 40 位元組，事先配置
   out=/絕對路徑                  # 預設是控制資料夾本身
   ```

3. 窗口結束後，`<out>/waitlog-<id>-<exe>-<pid>.events` 和 `.json` 出現（`.json` 最後寫，看到它就是寫完了）。

量測局用 `tools/video/msession.sh waitlog`（私人 repo），它會寫 `ctl`、等檔案、跑下面的分析。測試程式用 `tools/waitlog-test/run.sh`。

## 分析

```sh
python3 tools/waitlog/waitlog.py <錄到的 .json> [--focus <tid>|busiest|first] [--labels <檔案>] [--events]
```

- 每條執行緒：窗口內的 CPU 時間（Mach 的執行緒時間，窗口頭尾各讀一次）、記到的等待依路分開（server、alert 的 critical section／SRW／條件變數／WaitOnAddress、Sleep、keyed event）、yield、`other`（既不在 CPU 上、也不在記到的等待裡：沒記到的等待，例如 Vulkan 驅動裡的，或排不到 CPU）。
- 焦點執行緒（預設 CPU 最多的那條）：每次等待被誰結束（執行緒＋wineserver 請求名稱，或 ntdll 的鎖），次數、總時間、平均、p50、p95、最大；等的是什麼（handle 的型別、鎖的位址）；alert 從叫醒到再跑起來的延遲。
- `--check-roles <waittest 的 stdout>`：`waittest.exe roles` 每個角色是否拿到事先知道的答案。
- `requests-wine-11.18.txt` 是 wineserver 請求碼對名稱的表（從 Wine 11.18 的 `server_protocol.h` 匯出）。

## 檔案格式

`.events` 是一串 40 位元組的紀錄（小端）：

| 欄位 | 型別 | 內容 |
|---|---|---|
| start、end | u64 | `mach_absolute_time()`，換算用 `.json` 的 `timebase` |
| obj | u64 | handle、位址（最上面一個位元組：1 WaitOnAddress、2 critical section、3／4 SRW 獨占／共用、5 條件變數、6 barrier）、key、目標執行緒、timeout 或 yield 次數 |
| tid | u32 | 等待（或動作）的 Windows 執行緒 |
| waker | u32 | server 路：bit 31 有設是 Squark 的 wineserver 填的，bits 21–30 請求碼＋1（0 是逾時、計時器或 fd），bits 0–20 執行緒 id／4；1 是呼叫時就已經 signaled；0 是不知道。alert 路：叫醒它的執行緒 id，0 是不知道 |
| status | u32 | 回傳的 NTSTATUS（0x103 是窗口結束時還在等） |
| kind | u16 | 1 server wait、2 signal and wait、3／4 keyed wait／release、5 Sleep、6 可警示的 Sleep、7 yield、8 alert wait、9 alert send、10–13 SetEvent／PulseEvent／ReleaseSemaphore／ReleaseMutant、14 I/O completion |
| flags | u16 | 0x7f handle 數、0x80 WaitAll、0x100 alertable、0x200 有限 timeout、0x400 timeout 0、0x800 alert 早就到了、0x1000 窗口結束時還在等 |
