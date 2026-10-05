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
	};

	// Loads loader.json, writing a default one if it is missing or unreadable.
	Config& Load();
	Config& Get();
	bool Save();
}
