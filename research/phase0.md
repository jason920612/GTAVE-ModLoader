# 階段 0 研究報告（2026-10-05）

遊戲：GTA V Enhanced（Steam 3240220），執行期基址 `0x7ff7d38b0000`，SizeOfImage `0x5b45200`。
傾印：`research/dump/GTA5_Enhanced.dump.exe`（不進版控；`.text` 熵值 7.99 → 6.59 表示已解密）。
工具：`tools/dump.py`、`tools/scan.py`、`tools/disasm.py`、`tools/screen.py`、`tools/keys.py`。

## 1. 原生函式（natives）— 可行

- `InitNativeTables(scrProgram*)` 位於 RVA `0x91bf20`
  （特徵碼 `EB 2A 0F 1F 40 00 48 8B 54 17 10`，命中點 -0x2A）。
  - 讀 `+0x2C` 原生數量、`+0x40` 原生陣列（輸入為雜湊、輸出為處理函式指標）。
  - 註冊表：256 桶（以雜湊低位元組索引），節點的 next 指標經 XOR 混淆。
  - 找不到的雜湊填入預設處理函式（`lea r10, ...`）。
- 作法：配置假的 `scrProgram`，填入雜湊陣列後呼叫原函式 → 取得全部處理函式。
- 雜湊：Enhanced 的執行期雜湊與公開（原始）雜湊**不同**，需要 crossmap（約 6,596 筆，其中 6,435 筆不同）。
  社群 crossmap 自 2025-03 以來未變更 → Enhanced 更新時並未輪換雜湊，維護成本低。
- 呼叫慣例：`scrNativeCallContext`（0x80 bytes：ret@0x00、argc@0x08、args@0x10、向量修正@0x18..）。

## 2. 腳本執行緒 — 可行

- `RunScriptThreads(int ops)`：特徵碼 `BE 40 5D C6 00`，命中點 -0xA（RVA `0x920a40`）。
- `ScriptThreads`（`atArray<scrThread*>`）：`48 8B 05 ? ? ? ? 48 89 34 F8 48 FF C7 48 39 FB 75 97`，+3 RIP。
- 在原函式之後，借用一個遊戲腳本執行緒（`main_persistent` / `startup`）的身分執行模組的 tick。

## 3. 資源模組（檔案系統）— 可行，且不需要金鑰

| 用途 | 特徵碼命中 RVA |
|---|---|
| 跳過 `rpf.cache` 檢查（把目標函式改成 `ret`） | `0x4f73ae` |
| fiDeviceLocal OpenBulk / GetAttributes（vtable 1） | `0x10b110` / `0x10bc10` |
| fiDeviceLocal OpenBulk（vtable 2，DirectStorage 路徑） | `0x107f60` |
| 加密類型判斷 / 標頭解密（允許 OPEN 未加密 RPF） | `0x10fef2` / `0x10ff08` |
| ParseHeader（未加密 RPF 條目修正） | `0x10f5b0` |
| 初始掛載（加掛自訂 device，例如 `dlcpacks:/`） | `0x4f8987` |
| OpenArchive（資料夾當成封存檔開啟） | `0x10f210` |
| GetDevice | `0x10caf0` |

- NG 加密的 RPF 由遊戲本身解密；OPEN（未加密）RPF 只要繞過解密 → **完全不需要擷取金鑰**。
- **時機很關鍵**：檔案系統 Hook 必須在遊戲初始掛載**之前**安裝，比「等到解殼完成」還早。
  已知可行的做法：在 DllMain 裡 Hook `kernel32!GetSystemTimeAsFileTime`，
  第一次被呼叫時（已解殼、尚未掛載）再進行掃描與 Hook。Enhanced 的匯入表經過混淆，不能用 IAT Hook。

## 4. 線上安全閥

- 呼叫原生函式 `NETWORK_IS_SESSION_STARTED` / `NETWORK_IS_GAME_IN_PROGRESS` 判斷即可，不需要額外特徵碼。
- 另外檢查命令列是否帶 `-nobattleye`；沒有的話整個載入器停用。

## 5. 待第 3 階段確認

- DX12 `Present` Hook：遊戲經由 NVIDIA Streamline（`sl.interposer.dll`）建立交換鏈，需要確認 Hook 到的是真正的交換鏈。

## 參考（皆為 GPL，只參考做法，不複製程式碼）

- YimMenuV2（GPL-2.0）：invoker / InitNativeTables / RunScriptThreads 的做法。
- RageOpenV（GPL-3.0）：fiDevice 重導、rpf.cache、OPEN RPF 的做法。
