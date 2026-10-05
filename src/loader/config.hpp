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
		// Diagnostics only: skip hooking the game's script loop (mods will not run).
		bool debugDisableScriptHook = false;
	};

	// Loads loader.json, writing a default one if it is missing or unreadable.
	Config& Load();
	Config& Get();
	bool Save();
}
