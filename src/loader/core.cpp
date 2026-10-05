#include "core.hpp"

#include <Windows.h>

#include <chrono>
#include <filesystem>
#include <string_view>
#include <thread>

#include "config.hpp"
#include "crossmap_update.hpp"
#include "debug/watch.hpp"
#include "game/natives.hpp"
#include "game/pointers.hpp"
#include "game/script.hpp"
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
		if (config::Get().debugDisableScriptHook)
		{
			log::Warn("debugDisableScriptHook is set: script hook not installed");
			return;
		}
		if (game::script::InstallHooks(&OnTick))
			log::Info("script hook installed");
	}
}
