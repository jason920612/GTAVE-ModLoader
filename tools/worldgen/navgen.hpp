#pragma once
#include <cstdint>
#include <map>
#include <string>
#include <utility>
#include <vector>

#include "bounds.hpp"
#include "ynv.hpp"

// Pedestrians' navigation meshes for new land (research/phase0.md §61.10). Where pedestrians can walk is worked out from
// the collision triangles with Recast (voxels of 0.25 m), one 150 m square of the navmesh grid at a time with a border
// round it, so neighbouring squares agree along their common side; every edge on a side is then cut at the other
// square's vertices there and joined to the polygon across. Written as the game's navmesh[x][y].ynv (ynv.hpp), numbered
// for the loader's wider grid (loader.json worldNavGrid: y * 147 + x, x and y in 150 m squares from (-6000, -6000)).
//   Polygons: +0x00 flags (1 pavement, 2 road), +0x24 flags (road: 0x420, as the game's roads), +0x28 / +0x29 the centre
//   in the box (x 255), +0x2A eight directions with something in the way within 8 m (bit 0 north, then
//   anticlockwise), +0x2C the chunk it is in (x 16). Edges: the polygon across (<< 5) and its adjacency slot, the first
//   word with the free space beside the edge (0.25 m units, bits 22-26). The boxes (+0x18) are written in half metres;
//   the loader works them out again anyway (§61.8).
namespace worldgen::navgen
{
	enum class Surface : uint8_t
	{
		None,
		Ground,
		Pavement,
		Road
	};

	struct Settings
	{
		float cellSize = 0.25f, cellHeight = 0.1f;
		float agentHeight = 1.9f, agentRadius = 0.25f, agentClimb = 0.45f, maxSlope = 50.0f; // (m, degrees)
		float maxEdgeLength = 16.0f, maxError = 0.3f;                                       // (m)
		float polygonSpan = 15.0f; // (m) polygons stay within squares of this size (a tenth of a navmesh's), so their planes follow the ground
		int maxVertsPerPoly = 6;
		int pedDensity = 1; // (0-7) ambient pedestrians spawned on walkable ground and pavements
		int gridWidth = 147; // squares in a row of the grid
	};

	using Square = std::pair<int, int>; // 150 m squares from (-6000, -6000)

	struct Result
	{
		std::map<Square, ynv::Navmesh> navmeshes;
		size_t polygons = 0, joined = 0, cut = 0; // polygons; edges joined across squares; vertices put into edges there
	};

	// Every square the walkable triangles reach into. `surfaces`: per triangle of the mesh (None: not walkable).
	bool Build(const bounds::Mesh& mesh, const std::vector<Surface>& surfaces, const Settings& settings, Result& out, std::string& error);

	// navmesh[x][y].ynv: x and y in sectors of 50 m.
	std::string FileName(const Square& square);
}
