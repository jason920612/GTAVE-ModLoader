#pragma once
#include <filesystem>
#include <map>
#include <optional>
#include <string>
#include <vector>

#include "convert/archive.hpp"
#include "exporter.hpp"
#include "gamefiles.hpp"
#include "mapdata.hpp"

// The renderer's pack files (ue5/Source/GTAVRenderer/GTAVPackFormat.h, research/phase0.md §59), into <dir>:
//   models.gpk     every model the map uses, converted to Unreal's space, with its materials (textures by content hash)
//   textures.gpk   the game's texture data as it is, each distinct texture once
//   world.gpk      the map: archetypes, ymaps, entities (every level of detail), groups, interiors, grass
//   report.txt     what could not be converted, and why
namespace ue5export
{
	struct PackOptions
	{
		std::filesystem::path dir;
		std::filesystem::path gameDir;               // (each worker opens the game's files itself)
		const loader::convert::Decryptor* decrypt = nullptr;
		std::optional<Area> area;                    // a test area: only the models used there
		unsigned workers = 0;                        // 0: one per core, less two
		bool worldOnly = false;                      // world.gpk again; the models and textures as they are
	};

	struct PackStats
	{
		size_t models = 0, missingModels = 0, textures = 0, missingTextures = 0, entities = 0;
		uint64_t modelBytes = 0, textureBytes = 0, worldBytes = 0;
	};

	PackStats WritePack(const GameFiles& files, const mapdata::Map& map, const PackOptions& options, std::vector<std::string>& warnings);
	// globals.gpk alone (the renderer's textures from the game's shared dictionaries), into dir.
	void WriteGlobals(const GameFiles& files, const std::filesystem::path& dir, std::vector<std::string>& warnings);
}
