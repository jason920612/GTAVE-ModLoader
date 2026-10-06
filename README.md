# GTAVE-ModLoader

GTA V Enhanced（故事模式）的單一 DLL 模組載入器。

> 本專案僅供**故事模式**使用。載入器只在以 `-nobattleye` 啟動時運作，偵測到 GTA 線上模式會暫停所有模組。
> 不依賴 ScriptHookV；所有遊戲內部結構都是本專案自行逆向分析的結果（見 `research/phase0.md`）。

## 功能

- **放入即用**：遊戲資料夾放入 `version.dll`（小型代理）與 `ModLoader\ModLoader.dll`。
- **程式模組**：自動載入 `ModLoader\mods\*.dll`，每個模組有自己的資料夾；SDK 位於 `sdk/include/modloader`。
- **遊戲內管理**：F4 開啟管理視窗（模組、資源包、記錄、設定）；也可取代遊戲主畫面。
- **暫停選單「模組」分頁**：模組設定（開關、滑桿、自訂文字的選項列表）直接出現在遊戲的暫停選單。
- **模組選單（API v2）**：模組可以建立多層選單（開關、數值、選項列表、按鈕、文字）、可在遊戲內改鍵的熱鍵與畫面通知，
  顯示在 F4 視窗的「模組功能」分頁；在 MLOnLoad 註冊的開關、選項列表與 0–10 整數也會出現在暫停選單。
  按鈕與熱鍵的回呼在模組自己的腳本執行緒上執行，可以直接呼叫原生函式與 `Wait`。
- **修改器（`examples/trainer`）**：玩家（無敵、通緝、體力、超級跳、快跑、隱形）、載具（無敵、修理、改裝全滿、顏色）、
  武器、傳送（任務目標、地圖標記點、常用地點）、世界（時間、天氣、密度）與熱鍵；同時是選單 API 的範例。
- **DLC 包**：`ModLoader\mods\<名稱>\dlc.rpf` 會加入遊戲的 DLC 清單，支援未加密（OPEN）與加密（NG）的封裝檔；
  F4 的「資源包」頁可以停用個別的包或要求重新轉換。
- **舊版（Legacy）資源自動轉換**：含舊版貼圖字典（ytd v13）、模型（yft v162）、可繪物件（ydr／ydd v165）的包，會在第一次載入時轉成
  Enhanced 格式（ytd v5、yft v171、ydr／ydd v159）並存到 `ModLoader\cache`，載入畫面上會顯示進度條；之後直接使用快取。
  加密（NG）的舊版包透過遊戲本身的解密函式讀取；content.xml 中指向不存在檔案的項目會被移除（否則無法進入故事模式）。

## 建置

需要 Visual Studio 2022（MSVC）與 CMake 3.24 以上。

```
cmake -S . -B build
cmake --build build --config Release
```

輸出：`build/Release/version.dll`（代理）、`build/Release/ModLoader.dll`（載入器）、範例模組。

## 目錄

| 路徑 | 內容 |
| --- | --- |
| `src/stub` | `version.dll` 代理，只在 GTA5_Enhanced.exe 中載入 ModLoader.dll |
| `src/loader` | 載入器本體（模組、原生函式呼叫、介面、暫停選單、DLC 包） |
| `src/loader/convert` | 舊版 → Enhanced 資源轉換（RPF 讀寫、ytd、yft、效果參數表） |
| `sdk/include/modloader` | 模組 SDK |
| `examples` | 範例模組 |
| `tools` | 研究與離線轉換工具（Python） |
| `research/phase0.md` | 逆向分析筆記 |

## 目前限制

- 轉換 ytd、yft、ydr、ydd；舊版粒子效果（ypt）的格式在強化版大幅改變，目前無法轉換，會照原樣保留並顯示警告（遊戲不會載入它）。ypt 轉換已擱置，日後再做。
- 舊版貼圖參數依「從遊戲資料學到的名稱對照表」對應，表外的名稱依效果的貼圖順序補上。
- 資料有缺時採取降級並顯示警告（F4 資源包頁，滑鼠移到說明欄）：例如角色宣告了沒有模型的部位會被略過、
  content.xml 引用不存在的檔案會被移除、讀不了的檔案會被略過。
- 為了容納 add-on 內容，載入器會放大部分串流存放區的容量（例如角色外觀資料在原版已經滿了）。
- 遊戲版本 build stamp `0x6aa45f10` 上驗證；其他版本的特徵碼可能需要更新。

## 授權

MIT，見 `LICENSE`。GTA V 與相關商標屬於 Rockstar Games / Take-Two Interactive；本專案與其無關，也不包含任何遊戲檔案。
