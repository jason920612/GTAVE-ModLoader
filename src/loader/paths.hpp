#pragma once
#include <filesystem>

namespace loader::paths
{
	// <game>\ModLoader, <game>\ModLoader\mods, <game>\ModLoader\assets
	struct Paths
	{
		std::filesystem::path gameDir;
		std::filesystem::path root;
		std::filesystem::path mods;
		std::filesystem::path assets;
		std::filesystem::path config;
		std::filesystem::path log;
	};

	const Paths& Get();
	// Creates the folder layout; returns false if any folder could not be created.
	bool EnsureLayout();
}
