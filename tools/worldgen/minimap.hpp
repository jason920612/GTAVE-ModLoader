#pragma once
#include <array>
#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

#include "../ue5export/gamefiles.hpp"
#include "raster.hpp"

// The game's map of new land (research/phase0.md §61.26), in the game's own style:
//  - the radar's vector tiles: a drawable dictionary a supertile of the game's lattice (minimap.ymt: 9400 x 12492 m from
//    (-4500, 8000), 8 x 9 supertiles): the land, the water in depth bands, the foreground (buildings, roads) in 3 x 3
//    tiles; numbered on that lattice + 1000 (the game's own are 0-8), streamed and drawn beside the game's by the
//    loader (game/minimap.cpp);
//  - the pause map's pictures on the game's picture lattice (4500 m squares from (-4140, 8400), the game's 2 x 3): the
//    land's (512 px a side) and the sea's (1024, alpha: land opaque, water by its depth); a square the game has one of is
//    the game's with the new land drawn over it. The loader widens the game's grid to them.
namespace worldgen::minimap
{
	using P2 = std::array<float, 2>;

	struct Road
	{
		std::vector<P2> points;
		float halfWidth = 4;
		uint8_t grey = 180; // the radar's: 180 streets, 130 service roads and pedestrian streets, 99 tracks
	};

	struct Input
	{
		std::array<float, 4> box{};                  // what is built (x0, y0, x1, y1): the open sea round it
		const raster::Grid<float>* height = nullptr; // the ground over the box (under water its bottom; the sea level 0)
		std::vector<Road> roads;
		std::vector<std::vector<P2>> buildings; // footprints
	};

	struct Output
	{
		std::vector<std::array<int, 2>> supertiles; // the radar's (column, row on the game's lattice)
		std::vector<std::array<int, 2>> pictures;   // the pause map's (row, column on the game's picture lattice)
		size_t triangles = 0;
	};

	// The files into `out`: minimap_<column + 1000>_<row + 1000>.ydd; minimap_<name>_<row>_<column>.ytd and
	// minimap_sea_<name>_<row>_<column>.ytd, and the open sea's minimap_<name>_sea.ytd and minimap_sea_<name>_sea.ytd.
	bool Write(const ue5export::GameFiles& files, const Input& in, const std::string& name, const std::filesystem::path& out, Output& result,
	    std::string& error);
}
