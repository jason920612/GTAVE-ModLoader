#pragma once
#include <cmath>
#include <cstdint>

// Every random choice the build makes for a thing (a tree's model, a house's walls, a pothole) is keyed to where the thing
// is in the real world, not where the land puts it: two neighbouring lands show the same band of the real world round
// their common edge (research/phase0.md §65 - the crossing without a cut moves the player from one land's band into the
// other's in one frame), and what is in it must be the same in both. The real world's place of a point of the land: its
// game coordinates less the projection's origin there (world.json "projection" x0, y0 - one projection for all of
// Florida, the lands translations of each other in it).
namespace worldgen::seeds
{
	// (set once, from the world's projection, before anything is made)
	inline double g_x0 = 0, g_y0 = 0;
	inline void SetOrigin(double x0, double y0)
	{
		g_x0 = x0, g_y0 = y0;
	}
	inline double GX(double x)
	{
		return x - g_x0;
	}
	inline double GY(double y)
	{
		return y - g_y0;
	}

	inline uint32_t Mix(uint32_t h)
	{
		h ^= h >> 16, h *= 0x7feb352du, h ^= h >> 15, h *= 0x846ca68bu, h ^= h >> 16;
		return h;
	}

	// a number from a point's real-world place, rounded to 1/perMetre m (and `salt`: another stream for another choice)
	inline uint32_t Place(double x, double y, double perMetre, uint32_t salt = 0)
	{
		const auto i = static_cast<int64_t>(std::llround(GX(x) * perMetre)), j = static_cast<int64_t>(std::llround(GY(y) * perMetre));
		return Mix(static_cast<uint32_t>(i) * 73856093u ^ static_cast<uint32_t>(i >> 32) * 2246822519u ^ static_cast<uint32_t>(j) * 19349663u ^
		           static_cast<uint32_t>(j >> 32) * 3266489917u ^ salt * 83492791u);
	}

	// a lattice cell's number (cell [i, j] of `cell` m in the real world)
	inline uint32_t Cell(int64_t i, int64_t j, uint32_t salt = 0)
	{
		return Mix(static_cast<uint32_t>(i) * 73856093u ^ static_cast<uint32_t>(i >> 32) * 2246822519u ^ static_cast<uint32_t>(j) * 19349663u ^
		           static_cast<uint32_t>(j >> 32) * 3266489917u ^ salt * 83492791u);
	}
}
