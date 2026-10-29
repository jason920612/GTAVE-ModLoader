#pragma once
#include <filesystem>
#include <string_view>
#include <vector>

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

	// The world builder's lands, packs with a land.json each (worldgen's): in ModLoader\mods and in the folders of config
	// landFolders (another drive's - a land's folder there as in ModLoader\mods). The folders of config landFolders that are
	// there; every land's folder (a land in more than one: ModLoader\mods' first, then the list's order); a land's folder
	// (ModLoader\mods\<land> when none has it).
	std::vector<std::filesystem::path> LandRoots();
	std::vector<std::filesystem::path> LandFolders();
	std::filesystem::path LandDir(std::string_view land);
	// The lands' cache on a fast drive (config landCache; empty: none), and a land's folder there when its copy is whole -
	// every file of the land's own folder there, of its size and time, no dlc.rpf.new waiting beside them - else the land's
	// own folder (LandFolders and LandDir give the copy's).
	std::filesystem::path CacheRoot();
	std::filesystem::path Cached(const std::filesystem::path& folder);
}
