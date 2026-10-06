#pragma once
#include <filesystem>
#include <functional>
#include <string>
#include <vector>

#include "packs.hpp"
#include "xmlmerge.hpp"

// Replacement mods (research/phase0.md §25): a mod folder without a dlc.rpf holds files that replace the game's
// own (loose files, or .rpf archives holding them). They are packed into a generated DLC pack in
// ModLoader\cache\<name>\dlc.rpf: streaming files (models, textures, maps, ...) go into an archive registered as an
// overlay, so they take the place of the game's files with the same name; legacy ones are converted first. XML data
// files become per-entry overrides of the game's data files, and are also registered as DLC data files so entries
// the game does not have yet are added.
namespace loader::convert
{
	struct ReplacementFiles
	{
		std::vector<std::string> streaming; // file names (lower case) this mod replaces or adds
	};

	// Whether the folder holds files a replacement mod could contain.
	bool LooksLikeReplacementMod(const std::filesystem::path& dir);

	// Builds (or reuses) the generated pack. The mod's data file entries are added to `overrides`.
	PackResult PrepareReplacement(const std::string& name, const std::filesystem::path& dir, const Decryptor* decrypt,
	    const std::function<void()>& onConvert, xmlmerge::Overrides& overrides, ReplacementFiles& files);
}
