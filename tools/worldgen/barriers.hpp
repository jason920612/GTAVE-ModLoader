#pragma once
#include <array>
#include <cstddef>
#include <functional>
#include <vector>

#include "props.hpp"
#include "roads.hpp"
#include "world.hpp"

// Barriers along lines (research/phase0.md §63): OpenStreetMap's fences, walls, hedges and guard rails as the street
// pictures show them - chain link on galvanised posts (the untagged fences, Florida's commonest: round the stormwater
// ponds, the schools' fields, the substations and the yards behind the shops), board fences of weathered boards, walls
// of painted block, concrete retaining walls, W-beam guard rails on posts; hedges as rows of the game's clipped bushes.
// Models of our own: their meshes by material (positions in the world's coordinates); their collision with the roads'
// (kind 3: in the way, never walked on).
namespace worldgen::barriers
{
	struct Meshes
	{
		roads::Mesh mesh;     // chain link: the wire mesh (the game's cut-out fence shader; both faces)
		roads::Mesh steel;    // galvanised posts and rails
		roads::Mesh rail;     // guard rails' beams
		roads::Mesh boards;   // board fences
		roads::Mesh block;    // walls (painted block)
		roads::Mesh concrete; // retaining walls
	};

	constexpr uint8_t kCollisionKind = 3; // (roads::Collision's kinds: a barrier's)

	struct Stats
	{
		size_t fences = 0, walls = 0, hedges = 0, rails = 0, pieces = 0, crossing = 0;
		float metres = 0;
	};

	// `ground(x, y)`: the terrain's height; `onRoad(x, y)`: a road's asphalt or sidewalk there (a barrier's piece over it is
	// left out: the map's fences across driveways and gates, along a street drawn wider than it is). The pieces whose
	// middles lie in `box`; the hedges' bushes into `props`, the collision's triangles into `collision`.
	void Build(const std::vector<world::Barrier>& list, const std::function<float(float, float)>& ground,
	    const std::function<bool(float, float)>& onRoad, const std::array<float, 4>& box, Meshes& out, roads::Collision& collision,
	    std::vector<props::Prop>& props, Stats& stats);
}
