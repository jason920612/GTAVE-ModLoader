#include "menu.hpp"

#include <Windows.h>
#include <shellapi.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <format>
#include <string>

#include <imgui.h>

#include "../config.hpp"
#include "../convert/packs.hpp"
#include "../game/dlcpacks.hpp"
#include "../log.hpp"
#include "../mods.hpp"
#include "../paths.hpp"
#include "../state.hpp"
#include "overlay.hpp"

namespace loader::ui
{
	namespace
	{
		using Clock = std::chrono::steady_clock;

		std::atomic_bool g_menuOpen = false; // in-game window; toggled from the window thread
		bool g_restartNeeded = false;
		int g_selected = -1;
		Clock::time_point g_storyToastUntil{};
		bool g_storyToastShown = false;
		std::atomic_bool g_showOriginalLanding = false; // user fell back to the game's own landing page
		std::atomic_bool g_continueFailed = false;
		Clock::time_point g_continueDeadline{};

		const ImVec4 kAccent{0.30f, 0.69f, 0.31f, 1.0f};
		const ImVec4 kMuted{0.62f, 0.64f, 0.68f, 1.0f};
		const ImVec4 kWarn{0.95f, 0.70f, 0.20f, 1.0f};
		const ImVec4 kError{0.94f, 0.33f, 0.31f, 1.0f};

		std::string U8(const std::filesystem::path& p)
		{
			const auto s = p.u8string();
			return {s.begin(), s.end()};
		}

		void OpenFolder(const std::filesystem::path& dir)
		{
			std::error_code ec;
			std::filesystem::create_directories(dir, ec);
			ShellExecuteW(nullptr, L"open", dir.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
		}

		bool LandingReplaced()
		{
			return state::landing && !state::storyLoading && !g_showOriginalLanding && config::Get().replaceLandingPage;
		}

		void ContinueStory()
		{
			if (!state::canContinueStory)
			{
				g_showOriginalLanding = true;
				return;
			}
			state::storyLoading = true;
			g_continueFailed = false;
			g_continueDeadline = Clock::now() + std::chrono::seconds(25);
			state::storyRequested = true;
		}

		const char* StateLabel(mods::State s, ImVec4& color)
		{
			switch (s)
			{
			case mods::State::Disabled: color = kMuted; return "已停用";
			case mods::State::Failed: color = kError; return "載入失敗";
			case mods::State::Loaded: color = kMuted; return "已載入（等待故事模式）";
			case mods::State::Running: color = kAccent; return "執行中";
			case mods::State::Finished: color = kMuted; return "已結束";
			default: color = kError; return "已崩潰並停止";
			}
		}

		// ---- tabs ------------------------------------------------------------------------------

		void ModsTab()
		{
			const auto list = mods::Snapshot();
			auto& cfg = config::Get();

			if (ImGui::Button("開啟模組資料夾"))
				OpenFolder(paths::Get().mods);
			ImGui::SameLine();
			ImGui::TextColored(kMuted, "把模組 .dll 放進 ModLoader\\mods 後重新啟動遊戲");
			if (g_restartNeeded)
				ImGui::TextColored(kWarn, "設定已儲存，重新啟動遊戲後生效。");
			if (!mods::Loaded())
				ImGui::TextColored(kMuted, "正在載入模組…");
			else if (list.empty())
				ImGui::TextColored(kMuted, "目前沒有安裝任何模組。");

			const float detailHeight = ImGui::GetTextLineHeightWithSpacing() * 7;
			if (ImGui::BeginTable("mods", 5, ImGuiTableFlags_RowBg | ImGuiTableFlags_BordersInnerH | ImGuiTableFlags_ScrollY,
			        ImVec2(0, std::max(120.0f, ImGui::GetContentRegionAvail().y - detailHeight))))
			{
				ImGui::TableSetupColumn("啟用", ImGuiTableColumnFlags_WidthFixed);
				ImGui::TableSetupColumn("名稱", ImGuiTableColumnFlags_WidthStretch, 2.0f);
				ImGui::TableSetupColumn("版本", ImGuiTableColumnFlags_WidthStretch, 0.7f);
				ImGui::TableSetupColumn("作者", ImGuiTableColumnFlags_WidthStretch, 1.0f);
				ImGui::TableSetupColumn("狀態", ImGuiTableColumnFlags_WidthStretch, 1.4f);
				ImGui::TableSetupScrollFreeze(0, 1);
				ImGui::TableHeadersRow();

				for (int i = 0; i < static_cast<int>(list.size()); ++i)
				{
					const auto& m = list[i];
					ImGui::PushID(i);
					ImGui::TableNextRow();

					ImGui::TableNextColumn();
					bool enabled = !cfg.disabledMods.contains(m.fileName);
					if (ImGui::Checkbox("##on", &enabled))
					{
						if (enabled)
							cfg.disabledMods.erase(m.fileName);
						else
							cfg.disabledMods.insert(m.fileName);
						config::Save();
						g_restartNeeded = true;
					}

					ImGui::TableNextColumn();
					if (ImGui::Selectable(m.name.c_str(), g_selected == i, ImGuiSelectableFlags_SpanAllColumns | ImGuiSelectableFlags_AllowOverlap))
						g_selected = i;
					ImGui::TableNextColumn();
					ImGui::TextUnformatted(m.version.c_str());
					ImGui::TableNextColumn();
					ImGui::TextUnformatted(m.author.c_str());
					ImGui::TableNextColumn();
					ImVec4 color;
					const char* label = StateLabel(m.state, color);
					ImGui::TextColored(color, "%s", label);
					ImGui::PopID();
				}
				ImGui::EndTable();
			}

			ImGui::Separator();
			if (g_selected >= 0 && g_selected < static_cast<int>(list.size()))
			{
				const auto& m = list[g_selected];
				ImGui::Text("%s  %s", m.name.c_str(), m.version.c_str());
				ImGui::TextWrapped("%s", m.description.empty() ? "（沒有說明）" : m.description.c_str());
				ImGui::TextColored(kMuted, "檔案：%s", m.fileName.c_str());
				if (!m.error.empty())
					ImGui::TextColored(kError, "錯誤：%s", m.error.c_str());
				if (ImGui::Button("開啟此模組的資料夾"))
					OpenFolder(m.dir);
			}
			else
			{
				ImGui::TextColored(kMuted, "選擇一個模組以查看詳細資料。");
			}
		}

		void PacksTab()
		{
			const auto packs = game::dlcpacks::Snapshot();
			ImGui::PushStyleColor(ImGuiCol_Text, kMuted);
			ImGui::TextWrapped("%s", "放在 ModLoader\\mods\\<名稱>\\dlc.rpf 的 DLC 包。含舊版資源的包會自動轉成強化版格式，存在 ModLoader\\cache。");
			ImGui::PopStyleColor();
			if (ImGui::Button("開啟 mods 資料夾"))
				OpenFolder(paths::Get().mods);
			ImGui::SameLine();
			if (ImGui::Button("開啟轉換快取資料夾"))
				OpenFolder(paths::Get().root / L"cache");
			if (g_restartNeeded)
				ImGui::TextColored(kWarn, "設定已儲存，重新啟動遊戲後生效。");
			if (packs.empty())
			{
				ImGui::TextColored(kMuted, "目前沒有 DLC 包。");
				return;
			}
			auto& cfg = config::Get();
			if (ImGui::BeginTable("packs", 5, ImGuiTableFlags_RowBg | ImGuiTableFlags_BordersInnerH | ImGuiTableFlags_ScrollY))
			{
				ImGui::TableSetupColumn("啟用", ImGuiTableColumnFlags_WidthFixed);
				ImGui::TableSetupColumn("名稱", ImGuiTableColumnFlags_WidthStretch, 1.2f);
				ImGui::TableSetupColumn("狀態", ImGuiTableColumnFlags_WidthStretch, 1.4f);
				ImGui::TableSetupColumn("說明", ImGuiTableColumnFlags_WidthStretch, 2.2f);
				ImGui::TableSetupColumn("操作", ImGuiTableColumnFlags_WidthFixed);
				ImGui::TableSetupScrollFreeze(0, 1);
				ImGui::TableHeadersRow();
				for (int i = 0; i < static_cast<int>(packs.size()); ++i)
				{
					const auto& pack = packs[i];
					ImGui::PushID(i);
					ImGui::TableNextRow();
					ImGui::TableNextColumn();
					bool enabled = !cfg.disabledAssets.contains(pack.name);
					if (ImGui::Checkbox("##on", &enabled))
					{
						if (enabled)
							cfg.disabledAssets.erase(pack.name);
						else
							cfg.disabledAssets.insert(pack.name);
						config::Save();
						g_restartNeeded = true;
					}
					ImGui::TableNextColumn();
					ImGui::TextUnformatted(pack.name.c_str());
					ImGui::TableNextColumn();
					std::string note;
					if (!pack.enabled)
						ImGui::TextColored(kMuted, "已停用");
					else if (pack.state == convert::PackState::Failed)
					{
						ImGui::TextColored(kError, "轉換失敗，未載入");
						note = pack.error;
					}
					else if (!pack.registered)
						ImGui::TextColored(kError, "遊戲拒絕載入");
					else if (pack.state == convert::PackState::Converted)
					{
						ImGui::TextColored(kAccent, "已轉換（舊版 → 強化版）");
						note = std::format("{} 個檔案已轉換", pack.convertedFiles);
						if (!pack.warnings.empty())
							note += std::format("，{} 個警告（見記錄）", pack.warnings.size());
					}
					else
						ImGui::TextColored(kAccent, "已載入");
					ImGui::TableNextColumn();
					ImGui::TextWrapped("%s", note.c_str());
					ImGui::TableNextColumn();
					if (pack.state != convert::PackState::Native && ImGui::SmallButton("重新轉換"))
					{
						// The cached copy is rebuilt on the next start.
						std::error_code ec;
						std::filesystem::remove_all(paths::Get().root / L"cache" / pack.source.filename(), ec);
						g_restartNeeded = true;
					}
					ImGui::PopID();
				}
				ImGui::EndTable();
			}
		}

		// Shown while legacy packs are being converted (the game sits on its loading screen meanwhile).
		void ConversionProgress()
		{
			const auto p = convert::CurrentProgress();
			if (!p.active)
				return;
			const ImGuiViewport* vp = ImGui::GetMainViewport();
			const float width = vp->Size.x * 0.4f;
			ImGui::SetNextWindowPos(ImVec2(vp->Pos.x + (vp->Size.x - width) / 2, vp->Pos.y + vp->Size.y * 0.78f));
			ImGui::SetNextWindowSize(ImVec2(width, 0));
			ImGui::Begin("##convert", nullptr, ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoInputs | ImGuiWindowFlags_NoSavedSettings);
			ImGui::TextColored(kAccent, "正在轉換舊版模組：%s", p.pack.c_str());
			const float fraction = p.total ? static_cast<float>(p.done) / static_cast<float>(p.total) : 0.0f;
			const auto label = std::format("{} / {}", p.done, p.total);
			ImGui::ProgressBar(fraction, ImVec2(-1, 0), label.c_str());
			ImGui::TextColored(kMuted, "%s", p.file.c_str());
			ImGui::TextColored(kMuted, "只有第一次載入這個包時需要轉換，之後直接使用快取。");
			ImGui::End();
		}

		void LogTab()
		{
			const auto lines = log::Recent();
			ImGui::BeginChild("log", ImVec2(0, 0), ImGuiChildFlags_Borders, ImGuiWindowFlags_HorizontalScrollbar);
			for (const auto& line : lines)
			{
				const ImVec4 color = line.level == log::Level::Error ? kError
				                     : line.level == log::Level::Warn ? kWarn
				                     : line.level == log::Level::Debug ? kMuted
				                                                      : ImGui::GetStyleColorVec4(ImGuiCol_Text);
				ImGui::TextColored(color, "%s", line.text.c_str());
			}
			if (ImGui::GetScrollY() >= ImGui::GetScrollMaxY() - 4)
				ImGui::SetScrollHereY(1.0f);
			ImGui::EndChild();
		}

		void SettingsTab()
		{
			auto& cfg = config::Get();
			ImGui::SeparatorText("狀態");
			ImGui::Text("原生函式：%u 個可用（對照表 %u 筆）", state::nativesResolved.load(), state::crossmapEntries.load());
			ImGui::Text("遊戲模式：%s", state::online ? "GTA 線上（模組已暫停）" : state::story ? "故事模式" : "主畫面");
			if (ImGui::Button("開啟 ModLoader 資料夾"))
				OpenFolder(paths::Get().root);

			ImGui::SeparatorText("設定");
			bool changed = false;
			changed |= ImGui::Checkbox("以載入器介面取代遊戲主畫面", &cfg.replaceLandingPage);
			changed |= ImGui::Checkbox("每次啟動時更新原生函式對照表", &cfg.crossmapAutoUpdate);

			static const char* kKeys[] = {"F1", "F2", "F3", "F4", "F5", "F6", "F7", "F8", "F9", "F10", "F11", "F12"};
			int current = 3;
			for (int i = 0; i < 12; ++i)
				if (cfg.menuKey == kKeys[i])
					current = i;
			ImGui::SetNextItemWidth(ImGui::GetFontSize() * 6);
			if (ImGui::Combo("遊戲中開關選單的按鍵", &current, kKeys, 12))
			{
				cfg.menuKey = kKeys[current];
				changed = true;
			}
			if (changed)
				config::Save();
		}

		void Tabs()
		{
			if (ImGui::BeginTabBar("tabs"))
			{
				if (ImGui::BeginTabItem("模組"))
				{
					ModsTab();
					ImGui::EndTabItem();
				}
				if (ImGui::BeginTabItem("資源包"))
				{
					PacksTab();
					ImGui::EndTabItem();
				}
				if (ImGui::BeginTabItem("記錄"))
				{
					LogTab();
					ImGui::EndTabItem();
				}
				if (ImGui::BeginTabItem("設定"))
				{
					SettingsTab();
					ImGui::EndTabItem();
				}
				ImGui::EndTabBar();
			}
		}

		// ---- screens ---------------------------------------------------------------------------

		void LandingScreen()
		{
			const ImGuiViewport* vp = ImGui::GetMainViewport();
			ImGui::SetNextWindowPos(vp->Pos);
			ImGui::SetNextWindowSize(vp->Size);
			ImGui::PushStyleColor(ImGuiCol_WindowBg, ImVec4(0.06f, 0.07f, 0.08f, 1.0f));
			ImGui::Begin("##landing", nullptr,
			    ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoBringToFrontOnFocus);
			ImGui::PopStyleColor();

			const float pad = vp->Size.x * 0.04f;
			ImGui::SetCursorPos(ImVec2(pad, pad));
			ImGui::BeginGroup();
			ImGui::PushFont(nullptr, ImGui::GetFontSize() * 2.0f);
			ImGui::TextUnformatted("GTA V Enhanced");
			ImGui::PopFont();
			ImGui::TextColored(kAccent, "模組載入器");
			ImGui::EndGroup();

			const float sideWidth = vp->Size.x * 0.24f;
			ImGui::SetCursorPos(ImVec2(pad, pad + ImGui::GetFontSize() * 5));
			ImGui::BeginChild("side", ImVec2(sideWidth, -pad));
			const ImVec2 big(sideWidth, ImGui::GetFontSize() * 3.0f);
			ImGui::PushStyleColor(ImGuiCol_Button, kAccent);
			ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0.36f, 0.78f, 0.38f, 1.0f));
			if (ImGui::Button("繼續故事模式", big))
				ContinueStory();
			ImGui::PopStyleColor(2);
			ImGui::Spacing();
			if (ImGui::Button("退出遊戲", big))
				PostMessageW(GameWindow(), WM_CLOSE, 0, 0);

			const auto list = mods::Snapshot();
			const auto running = std::count_if(list.begin(), list.end(), [](const auto& m) {
				return m.state == mods::State::Loaded || m.state == mods::State::Running;
			});
			if (ImGui::Button("顯示原本的遊戲主畫面", big))
				g_showOriginalLanding = true;
			if (g_continueFailed)
				ImGui::TextColored(kWarn, "沒有成功進入故事模式。\n請再試一次，或改用原本的遊戲主畫面。");
			ImGui::Dummy(ImVec2(0, ImGui::GetFontSize()));
			ImGui::TextColored(kMuted, "已安裝 %zu 個模組，%lld 個已啟用", list.size(), static_cast<long long>(running));
			ImGui::TextColored(kMuted, "GTA 線上模式已停用（BattlEye 關閉中）");
			ImGui::TextColored(kMuted, "遊戲中按 %s 開啟模組管理", config::Get().menuKey.c_str());
			ImGui::EndChild();

			ImGui::SetCursorPos(ImVec2(pad * 2 + sideWidth, pad + ImGui::GetFontSize() * 5));
			ImGui::BeginChild("main", ImVec2(-pad, -pad), ImGuiChildFlags_Borders);
			Tabs();
			ImGui::EndChild();
			ImGui::End();
		}

		// Covers the landing page while it is being driven into story mode.
		void LoadingCover()
		{
			const ImGuiViewport* vp = ImGui::GetMainViewport();
			ImGui::SetNextWindowPos(vp->Pos);
			ImGui::SetNextWindowSize(vp->Size);
			ImGui::PushStyleColor(ImGuiCol_WindowBg, ImVec4(0.06f, 0.07f, 0.08f, 1.0f));
			ImGui::Begin("##cover", nullptr, ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoInputs | ImGuiWindowFlags_NoSavedSettings);
			ImGui::PopStyleColor();
			const char* text = "正在進入故事模式…";
			const ImVec2 size = ImGui::CalcTextSize(text);
			ImGui::SetCursorPos(ImVec2((vp->Size.x - size.x) * 0.5f, (vp->Size.y - size.y) * 0.5f));
			ImGui::TextColored(kAccent, "%s", text);
			ImGui::End();
		}

		void MenuWindow()
		{
			const ImGuiViewport* vp = ImGui::GetMainViewport();
			ImGui::SetNextWindowSize(ImVec2(vp->Size.x * 0.5f, vp->Size.y * 0.6f), ImGuiCond_FirstUseEver);
			ImGui::SetNextWindowPos(ImVec2(vp->Size.x * 0.25f, vp->Size.y * 0.2f), ImGuiCond_FirstUseEver);
			bool open = true;
			if (ImGui::Begin("模組載入器", &open, ImGuiWindowFlags_NoCollapse))
				Tabs();
			ImGui::End();
			if (!open)
				g_menuOpen = false;
		}

		void Toast(const char* id, const ImVec4& color, const char* text)
		{
			const ImGuiViewport* vp = ImGui::GetMainViewport();
			ImGui::SetNextWindowPos(ImVec2(vp->Size.x * 0.5f, vp->Size.y * 0.06f), ImGuiCond_Always, ImVec2(0.5f, 0.0f));
			ImGui::SetNextWindowBgAlpha(0.85f);
			ImGui::Begin(id, nullptr,
			    ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoInputs | ImGuiWindowFlags_NoFocusOnAppearing |
			        ImGuiWindowFlags_NoNav);
			ImGui::TextColored(color, "%s", text);
			ImGui::End();
		}
	}

	void ApplyStyle()
	{
		ImGui::StyleColorsDark();
		ImGuiStyle& s = ImGui::GetStyle();
		s.WindowRounding = 8.0f;
		s.ChildRounding = 6.0f;
		s.FrameRounding = 5.0f;
		s.GrabRounding = 5.0f;
		s.TabRounding = 5.0f;
		s.WindowPadding = ImVec2(16, 14);
		s.FramePadding = ImVec2(10, 6);
		s.ItemSpacing = ImVec2(10, 8);
		auto* c = s.Colors;
		c[ImGuiCol_WindowBg] = ImVec4(0.08f, 0.09f, 0.10f, 0.96f);
		c[ImGuiCol_ChildBg] = ImVec4(0.10f, 0.11f, 0.12f, 1.0f);
		c[ImGuiCol_Header] = ImVec4(0.30f, 0.69f, 0.31f, 0.35f);
		c[ImGuiCol_HeaderHovered] = ImVec4(0.30f, 0.69f, 0.31f, 0.50f);
		c[ImGuiCol_HeaderActive] = ImVec4(0.30f, 0.69f, 0.31f, 0.65f);
		c[ImGuiCol_Tab] = ImVec4(0.14f, 0.15f, 0.17f, 1.0f);
		c[ImGuiCol_TabHovered] = ImVec4(0.30f, 0.69f, 0.31f, 0.60f);
		c[ImGuiCol_TabSelected] = ImVec4(0.30f, 0.69f, 0.31f, 0.85f);
		c[ImGuiCol_CheckMark] = kAccent;
		c[ImGuiCol_Button] = ImVec4(0.18f, 0.19f, 0.21f, 1.0f);
		c[ImGuiCol_ButtonHovered] = ImVec4(0.26f, 0.28f, 0.30f, 1.0f);
		c[ImGuiCol_FrameBg] = ImVec4(0.16f, 0.17f, 0.19f, 1.0f);
		c[ImGuiCol_TitleBgActive] = ImVec4(0.12f, 0.13f, 0.14f, 1.0f);
	}

	void LoadFonts()
	{
		ImGuiIO& io = ImGui::GetIO();
		wchar_t fonts[MAX_PATH];
		GetWindowsDirectoryW(fonts, MAX_PATH);
		const auto dir = std::filesystem::path(fonts) / L"Fonts";
		// Microsoft JhengHei covers Traditional Chinese; glyphs are rasterised on demand.
		for (const auto* name : {L"msjh.ttc", L"msyh.ttc", L"segoeui.ttf"})
		{
			if (std::filesystem::exists(dir / name) && io.Fonts->AddFontFromFileTTF(U8(dir / name).c_str(), 20.0f))
				return;
		}
		io.Fonts->AddFontDefault();
	}

	void DrawFrame()
	{
		// Scale the whole UI with the output height (designed at 1080p).
		ImGui::GetStyle().FontScaleMain = std::max(0.75f, ImGui::GetIO().DisplaySize.y / 1080.0f);

		// Continue-story did not get past the landing page in time: give the home screen back.
		if (state::storyFailed.exchange(false))
			g_continueFailed = true;
		if (state::storyLoading && state::landing && Clock::now() > g_continueDeadline)
		{
			state::storyLoading = false;
			g_continueFailed = true;
			log::Warn("home screen: story mode did not start; showing the home screen again");
		}

		ConversionProgress();
		if (LandingReplaced())
			LandingScreen();
		else if (state::storyLoading && state::landing && !g_showOriginalLanding)
			LoadingCover();
		else if (g_menuOpen)
			MenuWindow();

		if (state::online)
			Toast("##online", kWarn, "偵測到 GTA 線上模式：所有模組已暫停");
		else if (state::story)
		{
			if (!g_storyToastShown)
			{
				g_storyToastShown = true;
				g_storyToastUntil = Clock::now() + std::chrono::seconds(8);
			}
			if (Clock::now() < g_storyToastUntil && !g_menuOpen)
			{
				const auto text = std::format("模組載入器已啟動 · 按 {} 開啟模組管理", config::Get().menuKey);
				Toast("##hello", kAccent, text.c_str());
			}
		}
	}

	bool WantsInput()
	{
		return LandingReplaced() || g_menuOpen;
	}

	unsigned MenuKey()
	{
		const auto& key = config::Get().menuKey;
		if (key.size() >= 2 && (key[0] == 'F' || key[0] == 'f'))
		{
			const int n = std::atoi(key.c_str() + 1);
			if (n >= 1 && n <= 12)
				return VK_F1 + n - 1;
		}
		return VK_F4;
	}

	void ToggleMenu()
	{
		if (!LandingReplaced())
			g_menuOpen = !g_menuOpen.load();
	}
}
