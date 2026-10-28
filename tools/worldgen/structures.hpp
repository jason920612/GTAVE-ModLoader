#pragma once
#include <array>
#include <cstdint>
#include <functional>
#include <string>
#include <unordered_set>
#include <vector>

#include "elevation.hpp"
#include "world.hpp"

// Buildings of our own (research/phase0.md §61.18): each footprint's measured shape. The roof is the laser scan's
// building returns over the footprint as a surface of few triangles (the greedy insertion of Garland and Heckbert:
// the footprint's own triangles first, then the measured point farthest from the surface, until every point is within a
// tolerance; Delaunay flips inside the outline), so that flat roofs, pitched ones, set-back towers, courtyards and
// domes all come out of one method. Before that the heights become planes (regions grown over the cells; the small
// ones - roof clutter - join their neighbours), so that a roof's faces are flat and its triangles few. The walls stand on the outline, from below the ground to the roof's edge. Without
// a measurement a house (the map's house kinds, or an unnamed building of a house's size) gets a hip roof - each point of
// the roof 22 degrees up from the nearest wall, its walls a storey high (or the map's height less the roof), the rise at
// most 3.5 m (Florida's houses: low hip roofs of shingle or tile) - made as a measured roof would be; others the map's
// height as a flat-roofed block.
namespace worldgen::structures
{
	using P2 = std::array<float, 2>;
	using P3 = std::array<double, 3>;

	struct Shape
	{
		std::vector<P3> vertices;                   // world positions
		std::vector<std::array<uint32_t, 3>> roof;  // anticlockwise seen from above
		std::vector<std::array<uint32_t, 2>> outline; // the roof's edge, anticlockwise: the walls stand under it
		float base = 0;     // the ground under the building (the walls reach `sink` below it)
		float height = 0;   // the main roof above the base
		float top = 0;      // the highest point above the base
		bool measured = false;
		bool pitched = false; // (no measurement: a house's hip roof made from its footprint)
		std::string kind;   // the map's building value
	};

	struct Options
	{
		float tolerance = 0.3f;      // the roof's largest height error (m)
		size_t maxTriangles = 800;   // a roof's triangles at most
		float flat = 0.8f;           // a roof whose middle 80 % of heights spans less than this is flat
		float sink = 0.6f;           // the walls go this far below the base
		float coverage = 0.3f;       // the share of the footprint with building returns a measurement needs
		float minPlane = 6;          // the smallest roof plane kept (m²); smaller ones join a neighbour
		// the share of the unmeasured houses with a near-rectangular footprint that get a gable roof - the ridge along the
		// long side, the short sides' walls up to it - the rest hip roofs (north Florida's street pictures: half and half;
		// the south's hurricane country mostly hip)
		float gableShare = 0;
		// buildings joined to a big one (its wings, the map's "yes" parts of it): no house roof of their own
		const std::unordered_set<const world::Building*>* attached = nullptr;
	};

	struct Stats
	{
		size_t measured = 0, flat = 0, shaped = 0, mapHeight = 0, skipped = 0, triangles = 0, pitched = 0;
	};

	// `ground` is the terrain's height (the base is the lowest of it under the outline).
	bool Make(const world::Building& b, elevation::Scan* scan, const std::function<float(float, float)>& ground, const Options& o, Shape& out,
	    Stats& stats);

	// Buildings the scan saw that the map has no footprint for (sheds, garages, cabanas, buildings the map lacks): its
	// building cells outside every known footprint (and a metre round them), opened by 1.5 m (eaves and slivers along
	// the footprints' sides go), pieces of at least `minArea` m² and a storey high (2.8 m) traced round (Douglas-Peucker,
	// 0.6 m) as footprints of kind "yes" with the scan's height. Within `box` (x0, y0, x1, y1), a kilometre at a time (a
	// piece is the block's that holds its first cell). The blocks are worked on all the cores: `open` opens a scan reader
	// for each worker.
	std::vector<world::Building> Missing(const std::function<void(elevation::Scan&)>& open, const std::vector<world::Building>& known,
	    const std::array<float, 4>& box, float minArea = 40);
}
