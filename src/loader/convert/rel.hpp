#pragma once
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

// Audio game data (.rel: game.dat151.rel, sounds.dat54.rel, ...) of replacement mods, merged per entry into the game's
// files (research/phase0.md §26). Mods usually ship the whole legacy file; only entries that differ from both the
// game's entry and the legacy game's own version of it are applied, so Enhanced's updates to the rest stay.
namespace loader::convert::rel
{
	struct Mod
	{
		std::string name;   // mod name, for messages
		std::string bytes;  // the mod's file
	};

	// The game's file with the mods' entries (the first mod in the list wins), or "" when nothing changes / the files
	// cannot be read.
	std::string Merge(std::string_view game, const std::vector<Mod>& mods, int& replaced, int& added, std::string& error);
}
