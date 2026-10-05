#pragma once
#include <filesystem>
#include <string>
#include <vector>

// DLC packs shipped by mods: ModLoader\mods\<name>\dlc.rpf is added to the game's DLC list
// (as if it were in dlcpacks). Archives may be unencrypted ("OPEN"). See research/phase0.md §11.
namespace loader::game::dlcpacks
{
	// Hooks the DLC list processing and the archive decryption. Call once the game is decrypted.
	bool InstallHooks();

	struct Pack
	{
		std::string name; // folder name (UTF-8)
		std::string path; // as given to the game (UTF-8): D:/.../ModLoader/mods/<name>/
		std::filesystem::path dir;
		bool enabled = true;
		bool registered = false; // the game accepted it
	};
	std::vector<Pack> Snapshot();
}
