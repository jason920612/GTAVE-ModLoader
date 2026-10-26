#pragma once
#include <array>
#include <cstdint>
#include <functional>
#include <vector>

#include "roads.hpp"
#include "world.hpp"

// The street's furniture and trees (research/phase0.md §61.17), the game's own props: palms at the world's tree
// points and along the avenues, street lights along roads with sidewalks (their arm over the road), traffic signal
// masts at the signalled junctions (where roads::Geometry puts them: the far right corner of each road coming in).
namespace worldgen::props
{
	using P2 = std::array<float, 2>;
	using P3 = std::array<float, 3>;

	struct Prop
	{
		uint32_t archetype = 0;
		P3 position{};
		float heading = 0; // radians: the model's x axis from east, anticlockwise
		float lodDist = 200;
		float scale = 1;
	};

	// `open(x, y)`: not on a road's asphalt or in a building; `ground(x, y)`: the terrain's height. Only what lies in
	// `box` (x0, y0, x1, y1). `palms`: the world's tree points and the avenues' palms (off when a laser scan places the
	// trees where they are).
	void Place(const world::World& w, const roads::Network& n, const std::vector<roads::Signal>& signals, const std::function<bool(float, float)>& open,
	    const std::function<float(float, float)>& ground, const std::array<float, 4>& box, std::vector<Prop>& out, bool palms = true);
}
