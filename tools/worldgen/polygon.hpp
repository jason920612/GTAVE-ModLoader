#pragma once
#include <array>
#include <cstdint>
#include <vector>

// Plane polygons (rings of points, the last not repeating the first) for the world builder.
namespace worldgen::polygon
{
	using P2 = std::array<float, 2>;

	// Signed area: positive anticlockwise.
	double Area(const std::vector<P2>& ring);
	bool Inside(const std::vector<P2>& ring, P2 p);
	float DistanceToSegment(P2 p, P2 a, P2 b);
	// The ring without repeated points and without points on a straight line between their neighbours (within
	// `tolerance` m), anticlockwise.
	std::vector<P2> Clean(const std::vector<P2>& ring, float tolerance);
	// Ear clipping of a simple ring: triangles (anticlockwise) as indices into the ring.
	std::vector<std::array<uint32_t, 3>> Triangulate(const std::vector<P2>& ring);
}
