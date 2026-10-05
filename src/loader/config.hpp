#pragma once
#include <set>
#include <string>

#include "log.hpp"

namespace loader::config
{
	struct Config
	{
		log::Level logLevel = log::Level::Info;
		std::string menuKey = "F4";
		std::set<std::string> disabledMods;   // file names in ModLoader\mods
		std::set<std::string> disabledAssets; // folder names in ModLoader\assets
		// Public -> Enhanced native hash table, refreshed on every launch (kept if the download fails).
		std::string crossmapUrl = "https://raw.githubusercontent.com/YimMenu/YimMenuV2/enhanced/src/game/gta/invoker/crossmap.txt";
		bool crossmapAutoUpdate = true;
		// Show the loader's home screen instead of the game's (online-focused) landing page.
		bool replaceLandingPage = true;
		// Diagnostics only: skip hooking the game's script loop (mods will not run).
		bool debugDisableScriptHook = false;
		// Research only: log writes to the landing page flow state (hardware breakpoints).
		bool debugWatchLanding = false;
		// Research only: arm watches listed in ModLoader\debug_watch.txt.
		bool debugWatchFile = false;
		// Research only: from startup, log writes to the pause menu screen array globals.
		bool debugWatchBoot = false;
		// Story-mode pause menu: the "Online" tab becomes a "Mods" tab with each mod's settings.
		bool pauseMenuModsTab = true;
	};

	// Loads loader.json, writing a default one if it is missing or unreadable.
	Config& Load();
	Config& Get();
	bool Save();
}
