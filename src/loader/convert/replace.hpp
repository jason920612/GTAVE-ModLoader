#pragma once
#include <filesystem>
#include <functional>
#include <map>
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
	// A file the game opens by its path (audio banks, Scaleform movies, whole data files, ...): replaced by the mod's
	// copy in ModLoader\cache\<mod>\files whenever the game opens a file of that name (game/datafiles).
	struct NamedFile
	{
		std::string name;   // file name, lower case
		std::string folder; // the folder (or archive, without ".rpf") it belongs in, lower case; "" = any
		std::filesystem::path file;
		std::string mod;
		bool merge = false; // audio game data (.rel): merged per entry into the game's file, all mods together
	};

	// A .gxt2 entry of a mod: replaces the game's text with that label hash in every text file, and is added to the
	// game's text file of the same name (`file`) when that file does not have it.
	struct TextEntry
	{
		std::string text;
		std::string file; // lower case, e.g. "global.gxt2"
		std::string mod;
	};

	struct ReplacementFiles
	{
		std::vector<std::string> streaming;      // file names (lower case) this mod replaces or adds
		std::vector<NamedFile> named;
		std::map<uint32_t, TextEntry> text;      // .gxt2 entries by label hash, merged into the game's text files
	};

	// A vehicle of a replacement mod that another mod (earlier by name) replaces too: this mod's files are renamed
	// (`from` -> `to`) and the vehicle is added as a new model. Its data entries come from the mod when it has them, or
	// are copied from the game's entries of `from` while the game loads them (game/datafiles).
	struct VehicleClone
	{
		std::string from, to;  // model names, lower case
		std::string mod;
		std::string device;    // the generated pack's device, e.g. "dlc_mlr1234abcd"
		std::string vehicles;  // the mod's own entries, already renamed ("" = copy the game's)
		std::string variation;
		std::string handling;
		std::string handlingId; // set when the mod brings its own handling for the clone
	};

	// Names of the streaming files a replacement mod holds (lower case).
	std::vector<std::string> ListStreamingFiles(const std::filesystem::path& dir, const Decryptor* decrypt);

	// Files of the vehicle model `name` (fragment, high detail fragment, textures).
	bool IsVehicleFileOf(const std::string& file, const std::string& name, std::string* suffix = nullptr);

	// DLC packs inside the .oiv packages of a mod folder, extracted to ModLoader\cache\<mod>\oiv\<pack>\dlc.rpf.
	struct OivPack
	{
		std::string name; // "<mod>-<pack folder name>"
		std::filesystem::path dir;
	};
	std::vector<OivPack> ExtractOivPacks(const std::string& mod, const std::filesystem::path& dir, std::vector<std::string>& warnings);

	// Whether the folder holds files a replacement mod could contain.
	bool LooksLikeReplacementMod(const std::filesystem::path& dir);

	// Builds (or reuses) the generated pack. The mod's data file entries are added to `overrides`. Vehicles in
	// `renames` (model name -> new name) are renamed and returned in `clones`.
	PackResult PrepareReplacement(const std::string& name, const std::filesystem::path& dir, const Decryptor* decrypt,
	    const std::function<void()>& onConvert, xmlmerge::Overrides& overrides, ReplacementFiles& files,
	    const std::map<std::string, std::string>& renames, std::vector<VehicleClone>& clones);
}
