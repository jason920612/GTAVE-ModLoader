#pragma once
#include <array>
#include <cstddef>
#include <functional>
#include <vector>

#include "props.hpp"
#include "roads.hpp"

// Power lines along the streets and country roads (research/phase0.md §63): Florida's carry their power overhead on
// wooden poles, one side of the road behind the kerb, about 40 m apart - the game's own poles (prop_telegraph_01b: a
// crossarm of four insulators 14 m up; now and then prop_telegraph_01a, a transformer under its crossarm) - with three
// wires on the crossarm and the communications cable lower down, sagging between them. The wires are the game's
// cables: its cable shader (cablemesh's) draws a cable from points along it, each point twice - a side each, the shader
// turns the pair to face the camera and keeps the line a pixel wide however far off -, the cable's direction as the
// points' normal, its half width and its sag under the straight line at the point as the first texture coordinates, how
// far along it in the colour's blue (red and green: the wind's phase, a cable's own).
namespace worldgen::powerlines
{
	struct Stats
	{
		size_t poles = 0, spans = 0, wires = 0;
	};

	// `clear(x, y)`: whether a pole may stand there (off the asphalt, out of the buildings and the water, by no other
	// post); `ground(x, y)`: the terrain's height. The poles that stand in `box`, and the wires between them (cable
	// vertices, see above; their positions in the world's coordinates).
	void Place(const roads::Network& n, const std::function<bool(float, float)>& clear, const std::function<float(float, float)>& ground,
	    const std::array<float, 4>& box, std::vector<props::Prop>& poles, roads::Mesh& wires, Stats& stats);
}
