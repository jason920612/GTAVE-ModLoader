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

## 10. 原生「模組」分頁與模組設定（2026-10-05，build 0x6aa45f10）

- 先前「分類被跳過／右欄進不去」是測試工具按鍵按住 0.15 秒觸發選單連發造成的假象，遊戲沒有目標頁 ID 白名單。
- 每個 CMenuScreen +0x28 指向處理類別雜湊，選單第一次開啟時由工廠表（0x5C8510）建立 C++ 處理物件存到 +0x00。
- 欄位建構 `0x5D0690(screenId, flag)`：每次顯示欄位（包含游標停在分類上時的右欄預覽）都會呼叫，從 pref 陣列讀值。
  顯示用的 pref 陣列 `0x3DFC2A0`，已存檔值陣列 `0x3DFC610`；兩者不同時該項以斜體顯示。
- 選單事件處理 `0x5EF9E0(event*, args*)`：事件 `0x610E6168` = 選項變更（args[0] pref、args[1] 新值，GFx number 型別 `&0x8F == 3`）。
  只有在分頁堆疊最上層（`0x3DFCBF8` 指標、`0x3DFCC00` 數量，16 位元組一筆）是 6（設定）時才呼叫 `0x5C7040(pref, value, 1)` 寫入。
- 畫面陣列依 id 排序（二分搜尋），在第一次開啟前追加新 id（最大 id 之後）的畫面可正常運作。
- 實作：分頁 42 改為設定版面；每個有設定的模組一個分類 + 一個新頁面；15 個安全 pref 欄位在頁面間共用
  （hook 0x5D0690 換入該模組的值，hook 0x5EF9E0 把變更寫回模組並存 settings.json）。
- 選項列表：`CMenuArray+0x20`（`0x3DFCF40`）atArray，每筆 0x18：`{u32 id; u32* labels @+8; u16 count @+0x10}`。
  項目 optionType = 列表 id（0 = 滑桿）；建構時對每個標籤雜湊查 TheText（`0x51D620`），所以文字覆寫表可直接提供選項文字。
  遊戲自己的 id 都 < 100；我們在第一次開啟前追加 id 100..250 的列表給 ML_SETTING_LIST 使用。

## 11. DLC 包與未加密 RPF（2026-10-05，build 0x6aa45f10）

- RPF7 標頭 `{magic 'RPF7', 項目數, 名稱長度（bit 28-30 名稱位移）, 加密}`；遊戲檔案皆為 NG（`0x0FEFFFFF`）。
- 封裝開啟 `0x10FCB0`：先查 rpf.cache（`0x1100E0(裝置, 路徑)`），命中時 `[0]` = 明文目錄（標頭+項目+名稱），`+9` = 已解密 → 完全不解密。
- 解密分派 `0xAAADC0(加密, 金鑰索引, 資料, 大小)` 與上下文初始化 `0xAA9FE0`：只接受 NG/AES，其他值（含 'OPEN'）執行 int3。
  **這兩個函式受執行檔保護，hook 會被還原**（實測開頭位元組恢復原狀）。
- 項目格式（讀檔 `0x110D60` 確認）：檔案 `u64 = 名稱偏移 | 壓縮大小<<16（0=未壓縮） | (資料位置/512)<<40 | 資源旗標 bit63`，`+8` 原始大小，`+0xC` 加密旗標；目錄 `+4 = 0x7FFFFF00`、`+8` 第一個子項、`+0xC` 子項數。
- DLC 清單：`0xC29A50` 讀 dlclist.xml → 每條路徑 `0xC29FC0(管理器, 路徑)`（接 `%sdlc.rpf` 掛載）→ `0xC2A1C0(管理器)` 處理（也會掃 `platform:/dlcPacks/`）。進入故事模式時會再處理一次。
- 實作：hook `0xC2A1C0` 先登記 `ModLoader/mods/<包>/`；hook 快取查詢，對未加密的包回傳我們讀好的明文目錄（加密值改為合法的 AES，檔案本身未加密所以不會真的解密）。
  `IS_DLC_PRESENT(joaat(nameHash))` 實測為 1。
- setup2.xml / content.xml 格式：由遊戲讀取自己的 DLC 時擷取（SSetupData、CDataFileMgr__ContentsOfDataFileXml）。
- 內嵌封裝檔（例如 OpenIV 產生的附加車輛包裡的 `x64/vehicles.rpf`）的快取查詢路徑是 `<setup2.xml 的 deviceName>:/<包內路徑>`；
  載入器讀外層目錄時一併登記（`setup2.xml` 若有壓縮，用遊戲自帶的 zlib1.dll 解壓）。明文目錄的加密值填 NG（與遊戲自身封裝相同）。
- 封裝檔 vtable `+0x2488E90`：OpenBulk `+0x10`、ReadBulk `+0x38` 只做位移換算後交給上層裝置，不解密。
- **資源格式版本**：Enhanced 的 `vehicles.rpf` 內 yft = 171、ytd = 5（1105 個）；舊版（Legacy）附加車輛為 yft 162、ytd 13，
  遊戲會登記模型（IS_MODEL_IN_CDIMAGE = 1）但不會載入資源。舊版資源需要轉成 Enhanced 格式才能用。

## 12. 資源容器（轉換第 1 階段，2026-10-05）

- 資源大小編碼（遊戲 `0x1579E0`，舊版與 Enhanced 相同，已用 HSV 實際解壓大小驗證）：
  `base = 0x2000 << (f & 0xF)`；`count = (f & 0x10) + ((f>>2)&0x18) + ((f>>5)&0x3C) + ((f>>10)&0x7E) + ((f>>17)&0x7F)`；
  `size = count*base`，另外 `f & 0x8000000/0x4000000/0x2000000/0x1000000` 分別再加 `base/16`、`base/8`、`base/4`、`base/2`。
  版本 = `(sys>>28)<<4 | (gfx>>28)`；sys 旗標算虛擬區（結構），gfx 旗標算實體區（圖像/頂點）。
- 版本檢查在讀檔之前，依據目錄項目的旗標：舊版（yft 162、ytd 13）不會被讀取。
- Enhanced 原生封裝內的資源：**每一頁分開壓縮且加密**（串流解壓 `+0x22CA236`，每個 z_stream 只有一頁）。
- 我們的未加密封裝：版本旗標若為 Enhanced 版本，遊戲會讀取資料並**直接 inflate（跳過 16 位元組 RSC7 標頭，不解密）**，
  所以轉換後的輸出可用「RSC7 標頭 + 單一 deflate 串流」，不需要產生 Enhanced 的加密分頁格式。
- 資源壓縮用的是遊戲內建 zlib（`0x11F1E0`），不是 Oodle（Oodle 解碼器在此期間沒被呼叫）。
- 資源倉庫：TxdStore 物件 `0x3ED61C8`（建構時版本參數 5），`+0x40` 項目陣列（每筆 0x18：物件指標、參考數、名稱雜湊、父索引），
  `+0x48` 旗標陣列，容量 `+0x10`（80200）。adder 的貼圖字典可用 joaat 名稱找到，物件位於資源結構區頁首。
- Enhanced 貼圖字典（記憶體中）：`+0x20` 名稱雜湊 atArray、`+0x30` 貼圖指標 atArray；每張貼圖 `0x80` 位元組
  （vtable、`+0x28` 名稱指標、`+0x30` 指向結構區內另一個物件、`+0x48/+0x50` 執行期 GPU 物件）。

## 13. 貼圖字典轉換（轉換第 2 階段，2026-10-05）

- Enhanced 貼圖放置建構函式 `0x1192DC0`（vtable `+0x2708AF0`，由 `0x1C1F610` 呼叫）：
  `+0x08` 區塊數 × `+0x0C` 區塊位元組 = 像素資料大小；`+0x18/+0x1A` 寬高、`+0x1C` 深度、`+0x1E` 維度（1 = 2D）、
  `+0x1F` DXGI 格式、`+0x22` mip 數、`+0x28` 名稱、`+0x30` → `+0x58` 的 0x28 位元組視圖物件（遊戲填入）、`+0x38` 像素資料。
- 舊版貼圖（0x90）：`+0x28` 名稱、`+0x50/+0x52` 寬高、`+0x54` 深度、`+0x56` 每列位元組、`+0x58` D3D9 格式（FourCC 或列舉）、
  `+0x5D` mip 數、`+0x70` 像素資料。字典與頁面表格式兩版相同（`+0x08` 頁面表：8 個 0、結構區頁數、圖像區頁數、頁面指標）。
- 像素資料：各 mip 緊接存放（實測大、小尺寸繪製都正確）；每張貼圖 4KB 對齊。
- **資源每一頁會被配置到各自獨立的記憶體**：任何物件或貼圖資料都不能跨頁。轉換時圖像區用同一尺寸的頁，
  每頁至少放得下最大的貼圖，依大小 first-fit 裝箱；結構區只用一頁。
- 驗證：HSV 的 4 張貼圖轉換後，用 DRAW_SPRITE 在遊戲中以不同大小繪製，全部正確（tools/convert_ytd.py）。
- 已解決（約一半啟動時 DLC 資源沒登記）：串流影像登記 `0xCE40E0` 掃描封裝檔時，用 vtable `+0x200` 組完整檔名，
  依賴名稱表後面的 u16 父目錄表（`[packfile+0x20]`）。正常解密路徑會配置並由 `0x110060` 填好；快取路徑則假設快取的
  目錄已附上這張表。我們的快取目錄原本沒有 → 讀到堆積殘值 → 登記名稱變成 `//mltex.ytd` 或亂碼。
  現在讀目錄時一併產生父目錄表，連續 4 次啟動都正確載入。
- 登記流程：`0xCE40E0` 先看 rpf.cache 內的登記快取（`0x10F960` 依序讀出封裝狀態，我們的包不在其中）→ 退回掃描，
  每個檔案呼叫 `0xCE3320(登記表, 輸出, 檔名, 索引, 影像…)`。
