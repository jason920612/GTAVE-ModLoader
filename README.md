# GTAVE-ModLoader

GTA V Enhanced（故事模式）的單一 DLL 模組載入器。

> 本專案僅供**故事模式**使用。載入器只在以 `-nobattleye` 啟動時運作，偵測到 GTA 線上模式會暫停所有模組。
> 不依賴 ScriptHookV；所有遊戲內部結構都是本專案自行逆向分析的結果（見 `research/phase0.md`）。

## 功能

- **放入即用**：遊戲資料夾放入 `version.dll`（小型代理）與 `ModLoader\ModLoader.dll`。
- **程式模組**：自動載入 `ModLoader\mods\*.dll`，每個模組有自己的資料夾；SDK 位於 `sdk/include/modloader`。
- **遊戲內管理**：F4 開啟管理視窗（模組、資源包、記錄、設定）；也可取代遊戲主畫面。
- **暫停選單「模組」分頁**：模組設定（開關、滑桿、自訂文字的選項列表）直接出現在遊戲的暫停選單。
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

- 轉換 ytd、yft、ydr、ydd；其他舊版資源（例如粒子 ypt）照原樣保留。
- 舊版貼圖參數依「從遊戲資料學到的名稱對照表」對應，表外的名稱依效果的貼圖順序補上。
- 為了容納 add-on 內容，載入器會放大部分串流存放區的容量（例如角色外觀資料在原版已經滿了）。
- 遊戲版本 build stamp `0x6aa45f10` 上驗證；其他版本的特徵碼可能需要更新。

## 授權

MIT，見 `LICENSE`。GTA V 與相關商標屬於 Rockstar Games / Take-Two Interactive；本專案與其無關，也不包含任何遊戲檔案。
