#pragma once
#include <string>
#include <vector>

#include "../convert/xmlmerge.hpp"

// Per-entry overrides of the game's XML data files (research/phase0.md §25). Data files are registered by name
// (handling.meta, vehicles.meta, ... of the base game and of every DLC pack); when one of them has entries that a mod
// overrides, the game is handed a merged copy (ModLoader\cache\datafiles) instead of the original.
namespace loader::game::datafiles
{
	// Hooks the data file registration. Call once the game is decrypted, before the game loads its data files.
	bool InstallHooks();

	// Sets the entries to override (before the game loads its data files; later calls replace the set).
	void SetOverrides(convert::xmlmerge::Overrides overrides);

	// Files merged so far, as "path: n entries".
	std::vector<std::string> Merged();
}
