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

## 6. 階段 2 補充驗證（2026-10-05）

- 原生函式註冊表 RVA `0x3ED4C20`：256 桶；節點 next@+0x00（XOR 節點位址混淆）、7 個 handler@+0x10、
  數量@+0x48、雜湊@+0x54+i*16（皆 XOR 混淆）。執行期共 6,748 個原生函式（`tools/natives_dump.py`）。
- 公開雜湊 → 執行期雜湊必須用 crossmap；`SYSTEM`（BUILTIN）類維持不變。
  以「處理函式位址順序」自行推導：各命名空間內順序一致率 96.6%，不足以作為正式資料（錯 3% 會呼叫錯函式）。
- 目前腳本執行緒：`TLS[_tls_index]+0x7A0`，執行中旗標 `+0x7A8`，另有全域副本（特徵碼 `ActiveThread`）。
- scrThread：id@+0x08、state@+0x18、名稱雜湊@+0x150（= joaat(名稱)）、名稱@+0x154。
- scrNativeCallContext：回傳指標@+0x00、參數數量@+0x08、參數陣列@+0x10（每個 8 bytes）。
- 很多原生函式處理常式被 Arxan 切碎（jmp 到第二個 .text），用 `tools/trace.py` 追蹤。
- 實測：hello_mod 在故事模式讀到玩家座標、F5 生成 Adder 並坐進駕駛座。

## 7. 主畫面「故事模式」入口（2026-10-05，硬體寫入監看 + 靜態分析）

- 主畫面（Gen9 landing page）是 C++ 原生 UI，原生函式（SHUTDOWN_AND_LOAD_MOST_RECENT_SAVE）對它無效。
- `landing_pre_startup` 腳本只做：`while (native1()) WAIT(0); while (!native5()) WAIT(0);` 然後啟動 `startup`。
- 主畫面的所有卡片都經過 **Gen9 Script Router**：待處理請求是字串
  `source=<SRCS_*>,mode=<SRCM_*>,argType=<SRCA_*>,arg=<...>`，存在 atString（RVA `0x472B5A8`）。
  - `SetRouterLink(const Link*)`（RVA `0x13C4F30`）：由 `{source@0, mode@8, argType@0x10, arg@0x18}` 組字串，已有請求時不覆蓋。
  - `ClearRouterLink()`（RVA `0x13C4F00`）。
  - 主畫面每幀在 `0x5368B0` 讀取請求：`mode==SRCM_LANDING_PAGE && argType==ENTRYPOINT_ID` → 開主畫面入口；
    其他 → `SetFlowState(0x18)`（RVA `0x10780`），保留請求讓後續轉場依 `mode` 決定去處。
  - 列舉：SRCS_LANDING_PAGE_SP=4；SRCM_FREE=1（線上）、SRCM_STORY=2；SRCA_NONE=1。
- ⚠️ 單獨呼叫 `SetFlowState(0x18)`（沒有請求）會走**線上**流程，被 BattlEye 擋下（已實測，勿用）。
- 載入器做法：在腳本執行緒呼叫 `SetRouterLink({4, 2, 1})`，回讀字串確認含 `mode=SRCM_STORY`，
  否則 `ClearRouterLink()` 並退回原本主畫面。實測 17 秒進入故事模式，43 個腳本、無 `MainTransition`。

## 8. 暫停選單（2026-10-05）

- 畫面 ID 列舉：解析器描述元在 RVA `0x2892B70` 附近（名稱陣列 `0x28939F0`，164 個）。MAP=0、INFO=1、GAME=5、SETTINGS=6、
  STATS=10、HEADER=28、SETTINGS_LIST=51…（完整表：`research/menu_ids.json`，不進版控）。
- 畫面陣列（堆積）：每個畫面 0x50 bytes `{atArray items @0, ..., id @+0x38, depth @+0x3C, flags @+0x40}`，依 id 排序。
  項目 0x28 bytes `{目標畫面 @0, 文字標籤雜湊 @4, 選項 atArray @0x10, 動作/設定編號 @0x20}`。
- 故事模式分頁列（HEADER 28）：MAP、INFO、STATS、SETTINGS、GAME、**42（線上）**、FRIENDS、GALLERY、STORE、REPLAY_EDITOR。
  42 的頁面只顯示「需要 BattlEye」。
- 文字：GXT2 表（`2TXG`，74,144 筆，{hash, offset} 依 hash 排序）在堆積中、頁對齊。
  線上分頁：標籤 `0x8D0A157E`、標題 `0x07B8D6CB`、內文 `0xD615A27B`。就地覆寫即可改名與顯示狀態。
  注意：這些標籤可能也用在其他（線上相關）畫面。

## 9. 原生暫停選單注入（2026-10-05，實驗 experimentalPauseMenu）

- `LoadPauseMenuData(1)`（RVA `0x5C5C00`）只把 pausemenu.xml 排入解析；同一執行緒稍後由解析器（`0xAE5A47`）填入
  全域選單物件 `0x3DFCF20`（screens atArray @+0x10）。
- **選單的執行期快取在「第一次開啟暫停選單」時建立**，不是啟動時。資料只要在第一次開啟前改好，C++ 就會照著用。
- CMenuScreen 0x50：items atArray @+0x08、id @+0x40、depth @+0x44。CMenuItem 0x28：target @0、label @4、
  條件 atArray @0x10、pref @0x20、optionType @0x21、action @0x22。
- 文字：TheText（特徵碼 `48 8D 0D ? ? ? ? 89 FA E8 ? ? ? ? 84 C0 74 ? 48 8D 0D`）+0x270 覆寫雜湊表可新增標籤（已實作）。
- 設定分類可否選取依「目標畫面 ID」決定（110/112/132 不行；139 語音聊天可選但頁面在故事模式整頁停用）。
- 在一般清單頁（通知 93）附加項目：可顯示、可操作；**但自訂 pref 編號會對應到其他真實設定的儲存位置**
  （218 的滑桿改到了 index 24）。在弄清楚設定值的存放/索引方式之前，不可使用自訂 pref。
