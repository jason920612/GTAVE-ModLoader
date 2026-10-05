#include "core.hpp"

#include <Windows.h>

#include <chrono>
#include <filesystem>

#include "config.hpp"
#include "crossmap_update.hpp"
#include "game/natives.hpp"
#include "game/pointers.hpp"
#include "game/script.hpp"
#include "log.hpp"
#include "mods.hpp"
#include "paths.hpp"

namespace loader::core
{
	namespace
	{
		using game::script::Joaat;

		// Story mode's always-running script: mods borrow its context.
		constexpr uint32_t kHostScript = Joaat("main_persistent");
		// Only present in GTA Online.
		constexpr uint32_t kOnlineScript = Joaat("freemode");
		constexpr uint64_t kNetworkIsSessionStarted = 0x9DE624D2FC4B603F;

		DWORD g_gameThreadId = 0;

		std::filesystem::path CrossmapPath()
		{
			return paths::Get().root / L"crossmap.txt";
		}
		bool g_online = false;
		bool g_hostSeen = false;
		game::natives::Invocation g_loaderCall;

		bool NetworkSessionStarted()
		{
			g_loaderCall.Begin(kNetworkIsSessionStarted);
			return game::natives::Call(g_loaderCall) == game::natives::CallStatus::Ok && g_loaderCall.result[0] != 0;
		}

		void SetOnline(bool online)
		{
			if (online == g_online)
				return;
			g_online = online;
			if (online)
				log::Warn("GTA Online detected: all mods are paused");
			else
				log::Info("left GTA Online: mods resume");
		}

		void OnTick()
		{
			if (!g_gameThreadId)
			{
				g_gameThreadId = GetCurrentThreadId();
				log::Info("first script tick on thread {}", g_gameThreadId);
				if (!crossmap::WaitForUpdate(std::chrono::seconds(15)))
					log::Warn("crossmap: download still running, using the existing file");
				game::natives::LoadCrossmap(CrossmapPath());
				game::natives::ResolveHandlers();
				mods::LoadAll();
			}
			else if (GetCurrentThreadId() != g_gameThreadId)
			{
				return; // mods only ever run on the thread they were loaded on
			}

			if (game::script::FindThread(kOnlineScript))
			{
				SetOnline(true);
				return;
			}

			game::scrThread* host = game::script::FindThread(kHostScript);
			if (!host)
				return;
			if (!g_hostSeen)
			{
				g_hostSeen = true;
				log::Info("story mode is running, starting mods");
			}

			game::script::ScopedThread scope(host);
			SetOnline(NetworkSessionStarted());
			if (!g_online)
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
		if (config::Get().debugDisableScriptHook)
		{
			log::Warn("debugDisableScriptHook is set: script hook not installed");
			return;
		}
		if (game::script::InstallHooks(&OnTick))
			log::Info("script hook installed");
	}
}
