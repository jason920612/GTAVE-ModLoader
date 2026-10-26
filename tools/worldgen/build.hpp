#pragma once
#include <array>
#include <filesystem>
#include <string>
#include <vector>

#include "../ue5export/gamefiles.hpp"
#include "catalog.hpp"
#include "world.hpp"

// The world compiler (research/phase0.md §61.16): a world description -> the files of its DLC pack (worldgen pack makes
// the pack): land and water (the coastline; the box's edges become coast: an island), ground heights, terrain tiles
// (models, collision), roads (models, collision, traffic nodes), buildings (with a laser scan: of our own, measured
// (structures.hpp); without one: the game's own, matched to the footprints), map and archetype files, pedestrians'
// navmeshes, population zones.
namespace worldgen::build
{
	struct Options
	{
		std::string name = "world";
		std::filesystem::path out;      // the pack's input folder
		std::filesystem::path zonesOut; // the zones mod's folder (popzone.ipl, zonebind.ymt); empty: none
		float tile = 200;               // terrain / road tile (m)
		float cell = 8;                 // terrain mesh cell (m)
		float classify = 4;             // land / water raster (m)
		float border = 300;             // how far in from the box's edges the land ends (m)
		std::array<float, 4> area{};    // only this part (x0, y0, x1, y1), for tests; all zero: everything
		std::filesystem::path scan;     // laser-scan rasters (tools/worldgen/lidar.py): measured ground, buildings of our own
		std::filesystem::path aerial;   // aerial pictures (tools/worldgen/aerial.py): the roofs' textures
		float datum = 0.6f;             // added to the scan's heights (NAVD88; the game's sea is at 0)
		int roofTexture = 1024;         // a tile's roof picture (pixels a side)
		std::filesystem::path roadsObj; // the roads' surfaces in the work box as an OBJ file (for checking)
		bool buildings = true, navmesh = true, paths = true;
	};

	struct Stats
	{
		size_t tiles = 0, terrainTriangles = 0, roadEdges = 0, roadTriangles = 0, buildings = 0, navmeshes = 0, pathNodes = 0, regions = 0, zones = 0;
		size_t generatedTiles = 0;
	};

	bool Run(const ue5export::GameFiles& files, const world::World& w, const std::vector<catalog::Item>& catalog, const Options& o, Stats& stats,
	    std::string& error);
}
