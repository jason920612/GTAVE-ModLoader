#pragma once
#include <array>
#include <functional>
#include <vector>

#include "elevation.hpp"
#include "props.hpp"

// Trees where the laser scan saw them (research/phase0.md §61.18): the canopy's height over the ground (vegetation
// returns, outside the buildings) smoothed, its peaks (each the highest within a reach that grows with its height) are
// the trees; how far the crown falls to half the peak's height is its radius. A narrow crown for its height is a palm,
// a wide one a broadleaf tree (Miami's figs, oaks, mahoganies); each becomes one of the game's models of the nearest
// height, scaled to the measured one; low growth becomes bushes.
namespace worldgen::trees
{
	struct Stats
	{
		size_t palms = 0, broadleaf = 0, bushes = 0;
	};

	// `open(x, y)`: a trunk may stand there (not on a road, not in a building); `ground(x, y)`: the terrain's height.
	void Place(elevation::Scan& scan, const std::function<bool(float, float)>& open, const std::function<float(float, float)>& ground,
	    const std::array<float, 4>& box, std::vector<props::Prop>& out, Stats& stats);
}
