#pragma once
#include <array>
#include <cstdint>
#include <string>
#include <vector>

#include "../ue5export/drawable.hpp"

// Collision bounds (research/phase0.md §61.9): a map's .ybn holds one at its root, a drawable at +0xC8. Read as
// triangles in world space, for working out where pedestrians can walk.
//   A bound: +0x10 its type, +0x14 radius, +0x20 / +0x30 its box's maximum / minimum (local space).
//   Composite (10): +0x70 the children, +0x78 their matrices (4 rows of 4 floats: x, y, z axes, position), +0xA0 count.
//   Geometry (4) / BVH (8): +0x88 polygons (16 bytes), +0x90 the vertices' quantum, +0xA0 their offset, +0xB0 the
//   vertices (int16 x3: offset + q x quantum), +0xD0 / +0xD4 vertex / polygon counts, +0xF0 the materials (8 bytes, the
//   first their type), +0x118 each polygon's material (a byte).
//   A polygon: its first byte's low 3 bits its kind. 0 triangle: u16 vertices at +4, +6, +8 (top bit a flag); 1 sphere:
//   centre vertex +2, radius (float) +4; 2 capsule and 4 cylinder: end vertices +2, +8, radius +4; 3 box: vertices +4
//   .. +0xA, alternate corners of the box (000, 110, 101, 011).
//   Box (3), sphere (0), capsule (1), cylinder (others): taken as their box.
namespace worldgen::bounds
{
	struct Mesh
	{
		std::vector<std::array<float, 3>> vertices;
		std::vector<std::array<uint32_t, 3>> triangles;
		std::vector<uint8_t> materials; // per triangle: the material's type (the game's material list index)
		std::array<size_t, 6> counts{}; // triangles, boxes, spheres, capsules, cylinders, bounds read as a box
	};

	// A 3x4 transform as the game stores it: rows x, y, z axes and the position (row vector times matrix).
	using Matrix = std::array<std::array<float, 3>, 4>;
	constexpr Matrix kIdentity{{{1, 0, 0}, {0, 1, 0}, {0, 0, 1}, {0, 0, 0}}};
	Matrix Multiply(const Matrix& inner, const Matrix& outer);

	// The bound at `at` (and its children), transformed by `toWorld`, added to `out`.
	bool Collect(const ue5export::drawable::View& view, uint64_t at, const Matrix& toWorld, Mesh& out, std::string& error);
}
