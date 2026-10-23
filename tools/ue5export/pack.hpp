#pragma once
#include <filesystem>
#include <map>
#include <optional>
#include <string>
#include <vector>

#include "exporter.hpp"
#include "gamefiles.hpp"
#include "mapdata.hpp"

// The renderer's pack files (ue5/Source/GTAVRenderer/GTAVPackFormat.h, research/phase0.md §59), into <dir>:
//   models.gpk     every model, converted to Unreal's space, with its materials (textures by content hash)
//   textures.gpk   the game's texture data as it is, each distinct texture once
//   world.gpk      where things are
//   report.txt     what could not be converted, and why
namespace ue5export
{
	struct PackOptions
	{
		std::filesystem::path dir;
		std::optional<Area> area; // a test area: its models and placements only
	};

	struct PackStats
	{
		size_t models = 0, missingModels = 0, textures = 0, missingTextures = 0, placements = 0;
		uint64_t modelBytes = 0, textureBytes = 0;
	};

	PackStats WritePack(const GameFiles& files, const mapdata::Map& map, const PackOptions& options, std::vector<std::string>& warnings);
}
