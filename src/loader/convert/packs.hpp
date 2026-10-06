#pragma once
#include <filesystem>
#include <string>
#include <vector>

#include "archive.hpp"

// Legacy DLC packs: a pack whose dlc.rpf contains legacy resources (texture dictionaries version 13,
// fragments version 162, drawables and drawable dictionaries version 165) is converted once into
// ModLoader\cache\<name>\dlc.rpf (unencrypted), and that copy is what the game loads. The original is never
// modified.
namespace loader::convert
{
	enum class PackState
	{
		Native,    // nothing to convert: loaded as it is
		Converted, // loaded from the cache
		Failed,    // conversion failed: not loaded
	};

	struct PackResult
	{
		PackState state = PackState::Native;
		std::filesystem::path dir;          // folder holding the dlc.rpf to load
		int convertedFiles = 0;
		bool fromCache = false;
		std::string error;
		std::vector<std::string> warnings;
	};

	// `onConvert` runs right before a conversion starts (not when the cached copy is used).
	PackResult PreparePack(const std::string& name, const std::filesystem::path& dir, const Decryptor* decrypt,
	    const std::function<void()>& onConvert);

	// Progress of the conversion currently running (for the loading-screen overlay).
	struct Progress
	{
		bool active = false;
		std::string pack;
		std::string file;
		int done = 0;
		int total = 0;
	};
	Progress CurrentProgress();

	// One legacy resource file (research aid): converted by its name and version like inside a pack.
	// Returns false with `error` when it is not a legacy resource this converter handles or the conversion failed.
	bool ConvertResourceFile(const std::string& name, const Bytes& data, Bytes& out, std::vector<std::string>& warnings, std::string& error);
}
