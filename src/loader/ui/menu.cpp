#include "../web/browser.hpp"
#include "menu.hpp"

#include <Windows.h>
#include <shellapi.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <deque>
#include <format>
#include <map>
#include <mutex>
#include <string>
#include <vector>

#include <imgui.h>

#include "../ai/ai.hpp"
#include "../config.hpp"
#include "../convert/packs.hpp"
#include "../game/dlcpacks.hpp"
#include "../game/dlss.hpp"
#include "../log.hpp"
#include "../mods.hpp"
#include "../paths.hpp"
#include "../state.hpp"
#include "notify.hpp"
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

		// ---- notifications and hotkeys ---------------------------------------------------------

		struct Notification
		{
			std::string source, text;
			Clock::time_point until;
		};
		std::mutex g_notifyMutex;
		std::deque<Notification> g_notifications;
		constexpr auto kNotifyTime = std::chrono::seconds(5);
		constexpr size_t kMaxNotifications = 5;

		std::atomic<int32_t> g_binding = -1; // hotkey item waiting for a key

		std::string KeyName(unsigned vk)
		{
			if (!vk)
				return "未設定";
			UINT scan = MapVirtualKeyW(vk, MAPVK_VK_TO_VSC);
			switch (vk)
			{
			case VK_INSERT: case VK_DELETE: case VK_HOME: case VK_END: case VK_PRIOR: case VK_NEXT:
			case VK_LEFT: case VK_RIGHT: case VK_UP: case VK_DOWN: case VK_DIVIDE: case VK_RCONTROL: case VK_RMENU:
				scan |= 0x100; // extended key
				break;
			}
			wchar_t name[64]{};
			if (!GetKeyNameTextW(static_cast<LONG>(scan << 16), name, 64))
				return std::format("按鍵 {}", vk);
			char utf8[192]{};
			WideCharToMultiByte(CP_UTF8, 0, name, -1, utf8, sizeof(utf8), nullptr, nullptr);
			return utf8;
		}

		void Notifications()
		{
			std::vector<Notification> shown;
			{
				std::lock_guard lock(g_notifyMutex);
				const auto now = Clock::now();
				while (!g_notifications.empty() && g_notifications.front().until < now)
					g_notifications.pop_front();
				shown.assign(g_notifications.begin(), g_notifications.end());
			}
			if (shown.empty())
				return;
			// Right edge below the money and wanted stars (the game's own feed is bottom left); newest at the bottom.
			const ImGuiViewport* vp = ImGui::GetMainViewport();
			ImGui::SetNextWindowPos(ImVec2(vp->Pos.x + vp->Size.x * 0.985f, vp->Pos.y + vp->Size.y * 0.22f), ImGuiCond_Always, ImVec2(1.0f, 0.0f));
			ImGui::SetNextWindowBgAlpha(0.0f);
			ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 0.0f);
			ImGui::Begin("##notify", nullptr,
			    ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoInputs | ImGuiWindowFlags_NoFocusOnAppearing |
			        ImGuiWindowFlags_NoNav | ImGuiWindowFlags_NoSavedSettings);
			const float width = ImGui::GetFontSize() * 18;
			for (size_t i = 0; i < shown.size(); ++i)
			{
				ImGui::PushID(static_cast<int>(i));
				ImGui::PushStyleColor(ImGuiCol_ChildBg, ImVec4(0.05f, 0.06f, 0.07f, 0.94f));
				ImGui::BeginChild("n", ImVec2(width, 0), ImGuiChildFlags_AutoResizeY | ImGuiChildFlags_AlwaysUseWindowPadding);
				if (!shown[i].source.empty())
					ImGui::TextColored(kAccent, "%s", shown[i].source.c_str());
				ImGui::PushTextWrapPos(0.0f);
				ImGui::TextUnformatted(shown[i].text.c_str());
				ImGui::PopTextWrapPos();
				ImGui::EndChild();
				ImGui::PopStyleColor();
				ImGui::PopID();
			}
			ImGui::End();
			ImGui::PopStyleVar();
		}

		// ---- mod menus ---------------------------------------------------------------------------

		size_t g_menuMod = SIZE_MAX;                                     // index into mods::All()
		std::map<size_t, std::vector<std::pair<int32_t, std::string>>> g_pageStack; // per mod: opened pages
		char g_filter[128] = "";

		bool Matches(const std::string& label)
		{
			if (!*g_filter)
				return true;
			// Case-insensitive for ASCII; other text as is.
			std::string a = label, b = g_filter;
			const auto lower = [](std::string& t) { std::transform(t.begin(), t.end(), t.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); }); };
			lower(a);
			lower(b);
			return a.find(b) != std::string::npos;
		}

		void MenuItemRow(const mods::ItemView& item, std::vector<std::pair<int32_t, std::string>>& stack)
		{
			ImGui::PushID(item.handle);
			ImGui::BeginDisabled(!item.enabled);
			const float width = ImGui::GetContentRegionAvail().x;
			switch (item.kind)
			{
			case mods::ItemKind::Page:
				if (ImGui::Selectable(std::format("{}  ›", item.label).c_str()))
				{
					stack.emplace_back(item.handle, item.label);
					g_filter[0] = 0;
				}
				break;
			case mods::ItemKind::Toggle:
			{
				bool on = item.value != 0;
				if (ImGui::Checkbox(item.label.c_str(), &on))
					mods::UiSetValue(item.handle, on ? 1.0f : 0.0f);
				break;
			}
			case mods::ItemKind::Number:
			{
				ImGui::SetNextItemWidth(width * 0.5f);
				const bool whole = item.step == std::floor(item.step) && item.min == std::floor(item.min);
				if (whole)
				{
					int v = static_cast<int>(item.value);
					if (ImGui::SliderInt(item.label.c_str(), &v, static_cast<int>(item.min), static_cast<int>(item.max)))
						mods::UiSetValue(item.handle, static_cast<float>(v));
				}
				else
				{
					float v = item.value;
					if (ImGui::SliderFloat(item.label.c_str(), &v, item.min, item.max, "%.2f"))
						mods::UiSetValue(item.handle, v);
				}
				break;
			}
			case mods::ItemKind::List:
			{
				ImGui::SetNextItemWidth(width * 0.5f);
				const int current = static_cast<int>(item.value);
				if (ImGui::BeginCombo(item.label.c_str(), current >= 0 && current < static_cast<int>(item.options.size()) ? item.options[current].c_str() : "?"))
				{
					for (int i = 0; i < static_cast<int>(item.options.size()); ++i)
						if (ImGui::Selectable(item.options[i].c_str(), i == current))
							mods::UiSetValue(item.handle, static_cast<float>(i));
					ImGui::EndCombo();
				}
				break;
			}
			case mods::ItemKind::Action:
				if (ImGui::Button(item.label.c_str(), ImVec2(width, 0)))
					mods::UiActivate(item.handle);
				break;
			case mods::ItemKind::Text:
				ImGui::TextWrapped("%s", item.label.c_str());
				break;
			case mods::ItemKind::Hotkey:
			{
				const bool binding = g_binding == item.handle;
				const auto text = binding ? std::string("按下新按鍵…（Esc 取消，Backspace 清除）") : KeyName(static_cast<unsigned>(item.value));
				if (ImGui::Button(std::format("{}##key", text).c_str(), ImVec2(width * 0.5f, 0)))
					g_binding = binding ? -1 : item.handle;
				ImGui::SameLine();
				ImGui::TextUnformatted(item.label.c_str());
				break;
			}
			}
			ImGui::EndDisabled();
			ImGui::PopID();
		}

		void ModMenusTab()
		{
			const auto list = mods::Snapshot();
			std::vector<size_t> withMenu;
			for (size_t i = 0; i < list.size(); ++i)
				if (list[i].hasMenu && list[i].state != mods::State::Failed && list[i].state != mods::State::Disabled)
					withMenu.push_back(i);
			if (withMenu.empty())
			{
				ImGui::TextColored(kMuted, "%s", mods::Loaded() ? "沒有提供選單的模組。" : "正在載入模組…");
				return;
			}
			if (std::find(withMenu.begin(), withMenu.end(), g_menuMod) == withMenu.end())
				g_menuMod = withMenu.front();

			ImGui::BeginChild("mods", ImVec2(ImGui::GetFontSize() * 11, 0), ImGuiChildFlags_Borders);
			for (const size_t i : withMenu)
				if (ImGui::Selectable(list[i].name.c_str(), g_menuMod == i))
				{
					g_menuMod = i;
					g_filter[0] = 0;
				}
			ImGui::EndChild();
			ImGui::SameLine();

			ImGui::BeginChild("page", ImVec2(0, 0), ImGuiChildFlags_Borders);
			const auto& mod = list[g_menuMod];
			auto& stack = g_pageStack[g_menuMod];
			// A page the mod removed: go back to the nearest one that still exists.
			while (!stack.empty() && !mods::PageExists(g_menuMod, stack.back().first))
				stack.pop_back();
			const auto items = mods::PageItems(g_menuMod, stack.empty() ? ML_ROOT_PAGE : stack.back().first);

			if (!stack.empty())
			{
				if (ImGui::Button("‹ 返回"))
				{
					stack.pop_back();
					g_filter[0] = 0;
				}
				ImGui::SameLine();
			}
			std::string path = mod.name;
			for (const auto& [h, label] : stack)
				path += "  ›  " + label;
			ImGui::TextColored(kAccent, "%s", path.c_str());
			if (mod.state == mods::State::Faulted)
				ImGui::TextColored(kError, "這個模組已崩潰並停止，選單項目不會再有反應。");
			else if (!state::story)
				ImGui::TextColored(kMuted, "進入故事模式後，按鈕和熱鍵才會生效。");
			ImGui::Separator();

			if (items.size() > 12)
			{
				ImGui::SetNextItemWidth(ImGui::GetFontSize() * 14);
				ImGui::InputTextWithHint("##filter", "搜尋…", g_filter, sizeof(g_filter));
			}
			ImGui::BeginChild("items");
			for (const auto& item : items)
				if (Matches(item.label) || item.kind == mods::ItemKind::Text)
					MenuItemRow(item, stack);
			if (items.empty())
				ImGui::TextColored(kMuted, "（這一頁沒有項目）");
			ImGui::EndChild();
			ImGui::EndChild();
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
					else if (pack.replacement && (pack.empty || pack.registered))
					{
						// Replacement mods: files packed into a generated pack, or only files / text replaced by name.
						ImGui::TextColored(kAccent, "已套用（替換型）");
						if (pack.convertedFiles)
							note = std::format("{} 個舊版檔案已轉換", pack.convertedFiles);
						if (!pack.warnings.empty())
							note += std::format("{}{} 個警告（滑鼠移到這裡查看）", note.empty() ? "" : "，", pack.warnings.size());
					}
					else if (!pack.registered)
						ImGui::TextColored(kError, "遊戲拒絕載入");
					else if (pack.state == convert::PackState::Converted)
					{
						ImGui::TextColored(kAccent, "已轉換（舊版 → 強化版）");
						note = std::format("{} 個檔案已轉換", pack.convertedFiles);
						if (!pack.warnings.empty())
							note += std::format("，{} 個警告（滑鼠移到這裡查看）", pack.warnings.size());
					}
					else
					{
						ImGui::TextColored(kAccent, "已載入");
						if (!pack.warnings.empty())
							note = std::format("{} 個警告（滑鼠移到這裡查看）", pack.warnings.size());
					}
					ImGui::TableNextColumn();
					ImGui::TextWrapped("%s", note.c_str());
					if (!pack.warnings.empty() && ImGui::IsItemHovered())
					{
						// What could not be converted as it is: degraded or left out.
						ImGui::BeginTooltip();
						ImGui::PushTextWrapPos(ImGui::GetFontSize() * 40);
						for (const auto& w : pack.warnings)
							ImGui::TextColored(kWarn, "%s", w.c_str());
						ImGui::PopTextWrapPos();
						ImGui::EndTooltip();
					}
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

		// The local text model: on / off, download, state.
		void AiSettings(bool& changed)
		{
			auto& cfg = config::Get();
			if (ImGui::Checkbox("啟用本機 AI 文字模型（模組可用來產生貼文、對話等內容）", &cfg.aiEnabled))
			{
				changed = true;
				ai::SettingChanged();
			}
			const ai::Status s = ai::GetStatus();
			ImGui::Indent();
			if (s.state == ai::State::Downloading)
			{
				ImGui::Text("下載中：%s", s.message.c_str());
				const float f = s.total ? static_cast<float>(static_cast<double>(s.done) / static_cast<double>(s.total)) : 0.0f;
				ImGui::ProgressBar(f, ImVec2(ImGui::GetFontSize() * 18, 0),
				    std::format("{:.0f} / {:.0f} MB", static_cast<double>(s.done) / 1e6, static_cast<double>(s.total) / 1e6).c_str());
			}
			else if (!ai::Installed())
			{
				ImGui::TextColored(kMuted, "需要下載 llama.cpp 與 Qwen3.5 4B 模型（約 2.8 GB，在 CPU 上執行，不使用顯示卡）。");
				if (ImGui::Button("下載模型"))
					ai::Install();
			}
			else
			{
				static const char* kStates[] = {"已關閉", "尚未下載", "下載中", "啟動中", "運作中", "發生錯誤"};
				const int i = static_cast<int>(s.state);
				ImGui::Text("狀態：%s%s", kStates[i >= 0 && i < 6 ? i : 5], s.queued ? std::format("（排隊 {} 個）", s.queued).c_str() : "");
			}
			if (s.state == ai::State::Failed)
			{
				ImGui::TextColored(kError, "%s", s.message.c_str());
				if (ImGui::Button("重試"))
					ai::Install();
			}
			ImGui::Unindent();
		}

		// DLSS (research §51): the newest DLSS from NVIDIA's store and the Super Resolution model.
		void DlssSettings(bool& changed)
		{
			auto& cfg = config::Get();
			const auto s = game::dlss::GetStatus();
			const auto letter = [](uint32_t preset) {
				const std::string l = game::dlss::PresetLetter(preset);
				return l.empty() ? std::string("預設") : l;
			};
			ImGui::SeparatorText("DLSS");
			changed |= ImGui::Checkbox("使用 NVIDIA 最新的 DLSS（重新啟動遊戲後生效）", &cfg.dlssUpgrade);
			ImGui::Indent();
			if (s.checked && s.storeSr.empty())
				ImGui::TextColored(kMuted, "NVIDIA 存放區沒有 DLSS（需要 NVIDIA 顯示卡驅動程式，NVIDIA App 會自動下載最新版）。");
			else if (s.checked)
				ImGui::TextColored(kMuted, "NVIDIA 存放區：超解析度 %s、畫格生成 %s；遊戲內建 %s", s.storeSr.c_str(), s.storeFg.empty() ? "無" : s.storeFg.c_str(),
				    s.gameSr.empty() ? "無" : s.gameSr.c_str());
			if (!s.loadedSr.empty())
				ImGui::TextColored(kMuted, "這次使用：超解析度 %s（%s）%s", s.loadedSr.c_str(), s.loadedSrFromStore ? "NVIDIA 存放區" : "遊戲內建",
				    s.loadedFg.empty() ? "" : std::format("、畫格生成 {}（{}）", s.loadedFg, s.loadedFgFromStore ? "NVIDIA 存放區" : "遊戲內建").c_str());
			if (cfg.dlssUpgrade != s.upgradeAtStart)
				ImGui::TextColored(kWarn, "重新啟動遊戲後生效。");
			ImGui::Unindent();

			static const char* kLabels[] = {"自動（建議）", "依照遊戲", "K：DLSS 4", "L：DLSS 4.5（超高效能用）", "M：DLSS 4.5（效能用）"};
			static const char* kValues[] = {"auto", "game", "K", "L", "M"};
			int current = 0;
			for (int i = 0; i < 5; ++i)
				if (cfg.dlssPreset == kValues[i])
					current = i;
			ImGui::SetNextItemWidth(ImGui::GetFontSize() * 14);
			if (ImGui::Combo("超解析度模型", &current, kLabels, 5))
			{
				cfg.dlssPreset = kValues[current];
				changed = true;
				game::dlss::PresetChanged();
			}
			ImGui::Indent();
			ImGui::TextColored(kMuted, "自動：DLAA、品質、平衡用 K，效能用 M，超高效能用 L（L、M 需要 DLSS 310.5 以上）。立即套用。");
			static const char* kModes[] = {"關閉", "效能", "平衡", "品質", "超高效能", "超高品質", "DLAA"};
			if (s.mode > 0 && s.mode < 7)
				ImGui::TextColored(kMuted, "遊戲目前：%s，使用模型 %s（遊戲要求 %s）", kModes[s.mode], letter(s.sentPreset).c_str(), letter(s.gamePreset).c_str());
			else
				ImGui::TextColored(kMuted, "遊戲目前沒有使用 DLSS（遊戲的 設定 > 影像 > 畫面調整 選 NVIDIA DLSS）。");
			ImGui::Unindent();
		}

		// The graphics mod (research §59): UE5 drawing the game's world, its quality and brightness.
		void Ue5Settings(bool& changed)
		{
			auto& cfg = config::Get();
			ImGui::SeparatorText("UE5 畫質");
			changed |= ImGui::Checkbox("以 Unreal Engine 5 繪製遊戲世界（重新啟動遊戲後生效）", &cfg.ue5Renderer);
			ImGui::Indent();
			if (cfg.ue5Renderer && (cfg.ue5Editor.empty() || cfg.ue5Project.empty() || cfg.ue5Pack.empty()))
				ImGui::TextColored(kWarn, "loader.json 還需要 ue5Editor、ue5Project、ue5Pack（tools/ue5_setup.py、ue5export pack）。");
			ImGui::TextColored(kMuted, "%s 鍵切換 UE5 與遊戲原本的畫面。需要遊戲的 NVIDIA DLSS。", cfg.ue5Key.c_str());
			ImGui::Unindent();
			static const char* kLabels[] = {"低", "中", "高（建議）", "極高", "電影級"};
			static const char* kValues[] = {"low", "medium", "high", "ultra", "cinematic"};
			int current = 2;
			for (int i = 0; i < 5; ++i)
				if (cfg.ue5Preset == kValues[i])
					current = i;
			ImGui::SetNextItemWidth(ImGui::GetFontSize() * 10);
			if (ImGui::Combo("畫質", &current, kLabels, 5))
			{
				cfg.ue5Preset = kValues[current];
				changed = true;
			}
			ImGui::Indent();
			ImGui::TextColored(kMuted, "解析度（其餘由 UE 放大）、陰影、全域光照、反射、繪製距離、近處燈光數量。立即套用。");
			ImGui::Unindent();
			{
				static const char* kGameLabels[] = {"依照遊戲", "品質", "平衡", "效能（建議）", "超高效能"};
				static const char* kGameValues[] = {"game", "quality", "balanced", "performance", "ultra"};
				int game = 3;
				for (int i = 0; i < 5; ++i)
					if (cfg.ue5GameResolution == kGameValues[i])
						game = i;
				ImGui::SetNextItemWidth(ImGui::GetFontSize() * 10);
				if (ImGui::Combo("遊戲本身的解析度", &game, kGameLabels, 5))
				{
					cfg.ue5GameResolution = kGameValues[game];
					changed = true;
				}
				ImGui::Indent();
				ImGui::TextColored(kMuted, "UE5 繪製世界時，遊戲自己的 3D 只用來提供行人和車輛：以較低的 DLSS 模式渲染可省下顯示卡。重新啟動遊戲後生效。");
				ImGui::Unindent();
			}
			ImGui::SetNextItemWidth(ImGui::GetFontSize() * 10);
			changed |= ImGui::SliderFloat("亮度跟隨遊戲", &cfg.ue5ExposureMatch, 0.0f, 1.0f, "%.2f");
			ImGui::Indent();
			ImGui::TextColored(kMuted, "1：畫面整體亮度跟遊戲原本的場景一致；0：完全依 UE 自己的曝光（光影比例永遠是 UE 的）。");
			ImGui::Unindent();
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
			changed |= ImGui::Checkbox("以載入器的瀏覽器取代手機與電腦的網頁", &cfg.replaceBrowser);
			changed |= ImGui::Checkbox("每次啟動時更新原生函式對照表", &cfg.crossmapAutoUpdate);
			AiSettings(changed);

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
			DlssSettings(changed);
			Ue5Settings(changed);
			if (changed)
				config::Save();
		}

		void Tabs()
		{
			if (ImGui::BeginTabBar("tabs"))
			{
				if (ImGui::BeginTabItem("模組功能"))
				{
					ModMenusTab();
					ImGui::EndTabItem();
				}
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
		web::Draw();
		if (!g_menuOpen)
			g_binding = -1;
		if (LandingReplaced())
			LandingScreen();
		else if (state::storyLoading && state::landing && !g_showOriginalLanding)
			LoadingCover();
		else if (g_menuOpen)
			MenuWindow();

		Notifications();
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
		return LandingReplaced() || g_menuOpen || web::IsOpen();
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

	bool MenuOpen()
	{
		return g_menuOpen;
	}

	void ToggleMenu()
	{
		if (!LandingReplaced())
			g_menuOpen = !g_menuOpen.load();
	}

	void Notify(std::string source, std::string text)
	{
		std::lock_guard lock(g_notifyMutex);
		g_notifications.push_back({std::move(source), std::move(text), Clock::now() + kNotifyTime});
		while (g_notifications.size() > kMaxNotifications)
			g_notifications.pop_front();
	}

	bool CaptureHotkey(unsigned vk)
	{
		const int32_t item = g_binding;
		if (item < 0)
			return false;
		if (vk == VK_ESCAPE)
			g_binding = -1;
		else if (vk != MenuKey())
		{
			mods::UiSetValue(item, vk == VK_BACK ? 0.0f : static_cast<float>(vk));
			g_binding = -1;
		}
		return true;
	}
}
