#pragma once
#include <filesystem>
#include <string>
#include <vector>

#include "../convert/archive.hpp"

// The fourth character's files in the loader's own pack (ModLoader\core\char4): the game scripts that see the officer
// (rewrite.hpp), made from the game's own archives, and the SP3_ stats (one per per-character stat of the game, so
// the officer has a stats slot of their own). Built at the DLC list, again only when the game's archives change.
namespace loader::char4
{
	// The scripts that are rewritten (research/char4-design.md §0.0 Q4, phase0.md §55.6 U9): shops, ATM, respawn,
	// blips, and every paid free-roam activity with its launcher.
	extern const std::vector<std::string> kAwareScripts;

	struct Generated
	{
		bool ok = false;                   // the stats file and at least one script are in place
		std::vector<std::string> scripts;  // the scripts rewritten (lower case)
		std::vector<std::string> warnings; // scripts left vanilla, and why
	};

	// Writes (or keeps) the files. `decrypt`: the game's archive decryption.
	Generated Generate(const std::filesystem::path& gameDir, const std::filesystem::path& outDir, const convert::Decryptor* decrypt);
}
