#pragma once
#include <array>
#include <filesystem>
#include <string>

#include "gamefiles.hpp"
#include "mapdata.hpp"

// Writes what the UE5 importer reads (research/phase0.md §58.4), into <out>\export:
//   models\<archetype>.gmesh   geometry of the highest level of detail (GTA's coordinates: metres, z up)
//   models\<archetype>.json    materials (effect, textures as "<dictionary>/<texture>", constants), bounds
//   textures\<dictionary>\<texture>.dds   the game's texture data as it is
//   areas\<name>.json          the entities of an area: archetype, position, rotation (x y z w, as stored), scale
namespace ue5export
{
	struct Area
	{
		std::string name;
		std::array<float, 4> bounds{}; // x0 y0 x1 y1
	};

	struct ExportStats
	{
		size_t entities = 0, models = 0, textures = 0, missingModels = 0, missingTextures = 0;
	};

	ExportStats ExportArea(const GameFiles& files, const mapdata::Map& map, const Area& area, const std::filesystem::path& out,
	    std::vector<std::string>& warnings);
}
