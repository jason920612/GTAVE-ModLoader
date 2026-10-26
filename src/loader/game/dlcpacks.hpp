#pragma once
#include <filesystem>
#include <string>
#include <vector>

#include "../convert/packs.hpp"

// DLC packs shipped by mods: ModLoader\mods\<name>\dlc.rpf is added to the game's DLC list
// (as if it were in dlcpacks). Archives may be unencrypted ("OPEN"). Packs with legacy resources are converted
// first (convert/packs.hpp). See research/phase0.md §11. A mod folder without a dlc.rpf that holds game files is a
// replacement mod: it is packed into a generated pack first (convert/replace.hpp, §25).
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
		bool replacement = false; // a replacement mod: loose files / archives packed into a generated pack
		bool enabled = true;
		bool registered = false; // the game accepted it
		bool heldBack = false;   // a world builder's sector that waits (game/sectors): mounted only when first wanted
		bool empty = false;      // a replacement mod with nothing to load (e.g. only DLC packs in its .oiv)
		convert::PackState state = convert::PackState::Native;
		int convertedFiles = 0;
		std::string error;
		std::vector<std::string> warnings;
	};
	std::vector<Pack> Snapshot();

	// A world builder's sector held back at the start (a thousand of them would each be opened and read then) mounted now,
	// as the game mounts packs at its start: its archives indexed, the pack registered, its setup read, its content
	// mounted - its change sets are then the caller's to execute. Game thread. "" when it is there (also when it was
	// already), else what went wrong.
	std::string MountLater(const std::string& name);

	// The game's NG decryption for reading archives, or nullptr when it was not found.
	const convert::Decryptor* Decryptor();
}
