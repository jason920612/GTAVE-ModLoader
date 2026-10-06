#pragma once
#include <filesystem>
#include <string>
#include <vector>

#include "../convert/packs.hpp"

// DLC packs shipped by mods: ModLoader\mods\<name>\dlc.rpf is added to the game's DLC list
// (as if it were in dlcpacks). Archives may be unencrypted ("OPEN"). Packs with legacy resources are converted
// first (convert/packs.hpp). See research/phase0.md §11.
namespace loader::game::dlcpacks
{
	// Hooks the DLC list processing and the archive decryption. Call once the game is decrypted.
	bool InstallHooks();

	struct Pack
	{
		std::string name; // folder name (UTF-8)
		std::string path; // as given to the game (UTF-8): D:/.../ModLoader/mods/<name>/
		std::filesystem::path source; // ModLoader\mods\<name>
		std::filesystem::path dir;    // folder of the dlc.rpf the game loads (source, or the converted copy)
		bool enabled = true;
		bool registered = false; // the game accepted it
		convert::PackState state = convert::PackState::Native;
		int convertedFiles = 0;
		std::string error;
		std::vector<std::string> warnings;
	};
	std::vector<Pack> Snapshot();

	// The game's NG decryption for reading archives, or nullptr when it was not found.
	const convert::Decryptor* Decryptor();
}
