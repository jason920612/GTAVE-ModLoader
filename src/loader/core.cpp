#include "core.hpp"

#include <Windows.h>

#include <chrono>
#include <filesystem>
#include <format>
#include <string>
#include <string_view>
#include <thread>

#include "config.hpp"
#include "crossmap_update.hpp"
#include "debug/watch.hpp"
#include "game/natives.hpp"
#include "game/pointers.hpp"
#include "game/script.hpp"
#include "game/dlcpacks.hpp"
#include "game/pools.hpp"
#include "game/pausemenu.hpp"
#include "game/text.hpp"
#include "game/text_override.hpp"
#include "log.hpp"
#include "mods.hpp"
#include "paths.hpp"
#include "state.hpp"
#include "ui/overlay.hpp"

namespace loader::core
{
	namespace
	{
		using game::script::Joaat;

		// Story mode's always-running script: mods borrow its context.
		constexpr uint32_t kHostScript = Joaat("main_persistent");
		// Only present in GTA Online.
		constexpr uint32_t kOnlineScript = Joaat("freemode");

		// Runs the game's landing page; continue-story is issued from its context.
		constexpr uint32_t kLandingScript = Joaat("landing_pre_startup");
		constexpr uint64_t kNetworkIsSessionStarted = 0x9DE624D2FC4B603F;
		constexpr uint64_t kDisableAllControlActions = 0x5F4B6931816E599B;

		DWORD g_gameThreadId = 0;
		bool g_hostSeen = false;
		game::natives::Invocation g_loaderCall;

		std::filesystem::path CrossmapPath()
		{
			return paths::Get().root / L"crossmap.txt";
		}

		template<class... Args>
		bool CallNative(uint64_t hash, Args... args)
		{
			g_loaderCall.Begin(hash);
			(g_loaderCall.Push(static_cast<uint64_t>(args)), ...);
			const auto status = game::natives::Call(g_loaderCall);
			if (status != game::natives::CallStatus::Ok)
				log::Error("loader native {:#018x} failed ({})", hash, static_cast<int>(status));
			return status == game::natives::CallStatus::Ok;
		}

		// Same request the landing page's Story Mode card submits to the Gen9 Script Router.
		void RequestStoryMode()
		{
			constexpr int32_t kSourceLandingPageSp = 4; // SRCS_LANDING_PAGE_SP
			constexpr int32_t kModeStory = 2;           // SRCM_STORY
			constexpr int32_t kArgNone = 1;             // SRCA_NONE
			auto& p = game::g_pointers;
			if (p.RouterLink->length)
			{
				log::Warn("home screen: a router request is already pending, not overriding it");
				return;
			}
			game::ScriptRouterLink link{};
			link.source = kSourceLandingPageSp;
			link.mode = kModeStory;
			link.argType = kArgNone;
			p.SetRouterLink(&link);

			// Never let a mis-built request through: anything but story mode could start GTA Online.
			const std::string_view built(p.RouterLink->data ? p.RouterLink->data : "", p.RouterLink->length);
			if (built.find("mode=SRCM_STORY") == std::string_view::npos)
			{
				p.ClearRouterLink();
				state::storyLoading = false;
				state::storyFailed = true;
				log::Error("home screen: router request came out as '{}'; cleared it", built);
				return;
			}
			log::Info("home screen: requested story mode ({})", built);
		}

		// ---- pause menu: the story-mode "Online" tab becomes the loader's "Mods" tab ----------

		// Text labels used by that tab (GXT2 hashes, same in every language).
		constexpr uint32_t kTabLabel = 0x8D0A157E;  // header tab
		constexpr uint32_t kTabTitle = 0x07B8D6CB;  // page title
		constexpr uint32_t kTabBody = 0xD615A27B;   // page text
		constexpr uint64_t kGetCurrentLanguage = 0x2BDD44CC428A7EAE;

		struct TabTexts
		{
			const char* label;
			const char* title;
			std::string body;
		};

		TabTexts BuildTabTexts(int language)
		{
			int active = 0, disabled = 0, failed = 0;
			for (const auto& m : mods::Snapshot())
			{
				switch (m.state)
				{
				case mods::State::Disabled: ++disabled; break;
				case mods::State::Failed:
				case mods::State::Faulted: ++failed; break;
				default: ++active; break;
				}
			}
			const auto& key = config::Get().menuKey;
			if (language == 9) // Traditional Chinese
			{
				auto body = std::format("已啟用 {} 個模組", active);
				if (disabled)
					body += std::format("，停用 {} 個", disabled);
				if (failed)
					body += std::format("，{} 個發生錯誤", failed);
				body += std::format("。按 {} 開啟模組管理。", key);
				return {"模組", "模組載入器", body};
			}
			if (language == 12) // Simplified Chinese
			{
				auto body = std::format("已启用 {} 个模组", active);
				if (disabled)
					body += std::format("，停用 {} 个", disabled);
				if (failed)
					body += std::format("，{} 个出错", failed);
				body += std::format("。按 {} 打开模组管理。", key);
				return {"模组", "模组加载器", body};
			}
			auto body = std::format("{} mod(s) active", active);
			if (disabled)
				body += std::format(", {} disabled", disabled);
			if (failed)
				body += std::format(", {} with errors", failed);
			body += std::format(". Press {} to manage mods.", key);
			return {"Mods", "Mod Loader", body};
		}

		void UpdatePauseMenuTab()
		{
			static ULONGLONG nextUpdate = 0;
			static std::atomic_bool locating = false;
			const ULONGLONG now = GetTickCount64();
			if (now < nextUpdate)
				return;
			nextUpdate = now + 1000;

			if (!game::text::Located())
			{
				// Scanning for the text table takes a moment; never do it on the game thread.
				if (!locating.exchange(true))
					std::thread([] {
						game::text::Locate(kTabLabel);
						locating = false;
					}).detach();
				return;
			}

			g_loaderCall.Begin(kGetCurrentLanguage);
			const int language = game::natives::Call(g_loaderCall) == game::natives::CallStatus::Ok ? static_cast<int>(g_loaderCall.result[0]) : 0;
			const auto texts = BuildTabTexts(language);
			const bool ok = game::text::Replace(kTabLabel, texts.label) && game::text::Replace(kTabTitle, texts.title) &&
			                game::text::Replace(kTabBody, texts.body);
			static bool logged = false;
			if (ok && !logged)
			{
				logged = true;
				log::Info("pause menu: Online tab now shows the loader (language {})", language);
			}
		}

		void SetOnline(bool online)
		{
			if (online == state::online)
				return;
			state::online = online;
			if (online)
				log::Warn("GTA Online detected: all mods are paused");
			else
				log::Info("left GTA Online: mods resume");
		}

		void FirstTick()
		{
			g_gameThreadId = GetCurrentThreadId();
			log::Info("first script tick on thread {}", g_gameThreadId);
			if (!crossmap::WaitForUpdate(std::chrono::seconds(15)))
				log::Warn("crossmap: download still running, using the existing file");
			game::natives::LoadCrossmap(CrossmapPath());
			game::natives::ResolveHandlers();
			mods::LoadAll();

			// The probe device and DXGI patching must not stall the game thread.
			std::thread([] {
				if (!ui::StartOverlay())
					log::Error("ui: overlay unavailable");
			}).detach();
		}

		void OnTick()
		{
			if (!g_gameThreadId)
				FirstTick();
			else if (GetCurrentThreadId() != g_gameThreadId)
				return; // mods only ever run on the thread they were loaded on

			if (game::script::FindThread(kOnlineScript))
			{
				SetOnline(true);
				return;
			}
			// Without settings to show, the tab falls back to the loader's status text.
			if (!game::pausemenu::Tick())
				UpdatePauseMenuTab();
			if (config::Get().debugWatchFile)
				debug::PollWatchFile();
			debug::FlushHits();

			game::scrThread* host = game::script::FindThread(kHostScript);
			game::scrThread* landing = host ? nullptr : game::script::FindThread(kLandingScript);
			state::landing = landing != nullptr;
			static bool watchArmed = false;
			if (landing && !watchArmed && config::Get().debugWatchLanding)
			{
				watchArmed = true;
				std::thread([] {
					debug::ArmWriteWatches({{0x3DE3030, 2}, {0x29C7D1C, 4}, {0x29C7D30, 4}, {0x3DFA798, 8}});
				}).detach();
			}
			state::story = host != nullptr;
			if (landing && state::storyRequested.exchange(false))
				RequestStoryMode();
			if (!host)
				return;
			if (!g_hostSeen)
			{
				g_hostSeen = true;
				state::storyLoading = false;
				log::Info("story mode is running, starting mods");
			}

			game::script::ScopedThread scope(host);
			g_loaderCall.Begin(kNetworkIsSessionStarted);
			SetOnline(game::natives::Call(g_loaderCall) == game::natives::CallStatus::Ok && g_loaderCall.result[0] != 0);
			// The loader menu owns keyboard/mouse while it is open.
			if (ui::CapturesInput())
				CallNative(kDisableAllControlActions, 0);
			if (!state::online)
				mods::Tick();
		}
	}

	void StartBackgroundTasks()
	{
		crossmap::StartUpdate(CrossmapPath());
	}

	void OnGameUnpacked()
	{
		if (!game::ResolvePointers())
		{
			log::Error("this game build is not supported yet; loader stays inactive");
			return;
		}
		state::canContinueStory = game::g_pointers.SetRouterLink && game::g_pointers.ClearRouterLink;
		if (!state::canContinueStory)
			log::Warn("landing page story entry point not found; the home screen will offer the original landing page instead");
		game::pools::InstallHooks();
			game::dlcpacks::InstallHooks();
		if (config::Get().pauseMenuModsTab && game::text_override::Init())
			game::pausemenu::InstallHooks();
		if (config::Get().debugWatchBoot)
		{
			// Pause menu screen array: data pointer and count (see research/phase0.md).
			// Not joined: this runs under the loader lock, the thread starts once it is released.
			// Hardware breakpoints are per thread: keep arming threads created during startup.
			std::thread([] {
				for (int i = 0; i < 600; ++i)
				{
					debug::ArmWriteWatches({{0x3DFCF30, 8}, {0x3DFCF38, 2}});
					Sleep(100);
				}
			}).detach();
		}
		if (config::Get().debugDisableScriptHook)
		{
			log::Warn("debugDisableScriptHook is set: script hook not installed");
			return;
		}
		if (game::script::InstallHooks(&OnTick))
			log::Info("script hook installed");
	}
}
