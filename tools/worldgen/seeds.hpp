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

	// a number from a place in the real world's plane (metres, as GX and GY give them), rounded to 1/perMetre m (and `salt`:
	// another stream for another choice)
	inline uint32_t PlaceAt(double gx, double gy, double perMetre, uint32_t salt = 0)
	{
		const auto i = static_cast<int64_t>(std::llround(gx * perMetre)), j = static_cast<int64_t>(std::llround(gy * perMetre));
		return Mix(static_cast<uint32_t>(i) * 73856093u ^ static_cast<uint32_t>(i >> 32) * 2246822519u ^ static_cast<uint32_t>(j) * 19349663u ^
		           static_cast<uint32_t>(j >> 32) * 3266489917u ^ salt * 83492791u);
	}

	// the same from a point of the land (the world's points are the projection's lattice of 1/1024 m, floats exactly: the
	// same place gives the same number in every land - a point worked out from them in floats of the land's coordinates does
	// not quite: Middle, Between)
	inline uint32_t Place(double x, double y, double perMetre, uint32_t salt = 0)
	{
		return PlaceAt(GX(x), GY(y), perMetre, salt);
	}

	// the middle of points of the land (their mean) in the real world's plane, in doubles: the same in every land
	struct G
	{
		double x = 0, y = 0;
	};
	template <typename Points> inline G Middle(const Points& points)
	{
		G m;
		for (const auto& p : points)
			m.x += GX(p[0]), m.y += GY(p[1]);
		if (!points.empty())
			m.x /= static_cast<double>(points.size()), m.y /= static_cast<double>(points.size());
		return m;
	}
	// the point a share `t` of the way from a to b in the real world's plane
	template <typename P> inline G Between(const P& a, const P& b, double t = 0.5)
	{
		return {GX(a[0]) + (GX(b[0]) - GX(a[0])) * t, GY(a[1]) + (GY(b[1]) - GY(a[1])) * t};
	}
	inline uint32_t PlaceAt(const G& g, double perMetre, uint32_t salt = 0)
	{
		return PlaceAt(g.x, g.y, perMetre, salt);
	}

	// a lattice cell's number (cell [i, j] of `cell` m in the real world)
	inline uint32_t Cell(int64_t i, int64_t j, uint32_t salt = 0)
	{
		return Mix(static_cast<uint32_t>(i) * 73856093u ^ static_cast<uint32_t>(i >> 32) * 2246822519u ^ static_cast<uint32_t>(j) * 19349663u ^
		           static_cast<uint32_t>(j >> 32) * 3266489917u ^ salt * 83492791u);
	}
}
