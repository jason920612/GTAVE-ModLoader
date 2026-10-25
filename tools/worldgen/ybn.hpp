#pragma once
#include <array>
#include <cstdint>
#include <string>
#include <vector>

#include "convert/resource.hpp"

// Writing a map's collision file (.ybn, research/phase0.md §61.11): a composite bound (type 10) of BVH bounds (type 8),
// one per part, in world space (identity matrices). Resource version 0x2B, the virtual block only.
//   Composite (0xB0): +0x08 page map (the root's only: the game fixes +0x08 of every bound up as a pointer), +0x10 type,
//   +0x14 radius, +0x20 / +0x30 box max / min (w: margin, 1), +0x40 sphere centre, +0x50 the box's centre, +0x60 (1,1,1,
//   part count), +0x70 children, +0x78 / +0x80 their matrices (rows of 4 floats), +0x88 their boxes (32 bytes), +0x90 /
//   +0x98 their type and include flags (8 bytes), +0xA0 count, capacity, +0xA8 a BVH over the children (more than 5).
//   Every box is padded by the margin (0.005 m), as in the game's files: a ray along the face of a tight box misses it.
//   BVH bound (0x150): the same head, +0x84 vertex count, +0x88 polygons (16 bytes: area with the kind in its low 3
//   bits, u16 vertices, int16 neighbours across each edge or -1), +0x90 quantum, +0xA0 centre, +0xB0 vertices (int16 x3:
//   centre + q x quantum), +0xD0 / +0xD4 vertex and polygon counts, +0xF0 materials (8 bytes, the first the type in
//   common:/data/materials/materials.dat), +0x118 a material per polygon, +0x120 material count, +0x130 its BVH,
//   +0x140 0xFFFF.
//   BVH (0x80): +0x00 nodes, count, capacity, +0x20 / +0x30 min / max, +0x40 centre, +0x50 1 / quantum, +0x60 quantum
//   (half the extent / 32767), +0x70 subtree headers, u16 count and capacity. A node (16 bytes): int16 min and max,
//   then (subtree size, 0) or a leaf's (first polygon, count up to 4); polygons are stored in leaf order. A subtree header
//   (16 bytes): int16 box, its first node and the node after it, one per highest subtree of at most 128 nodes.
namespace worldgen::ybn
{
	struct Part
	{
		std::vector<std::array<float, 3>> vertices;
		std::vector<std::array<uint32_t, 3>> triangles; // anticlockwise seen from the outside
		std::vector<std::array<uint8_t, 8>> materials;
		std::vector<uint8_t> triangleMaterials; // index into materials, per triangle
	};

	// A material of the given type (materials.dat row: 1 concrete, 4 tarmac, 47 grass, 174 concrete pavement...).
	std::array<uint8_t, 8> Material(uint8_t type);

	bool Write(const std::vector<Part>& parts, loader::convert::Bytes& out, std::string& error);

	// The box of a written file's root (for the map cache's BoundsStore entry).
	void Box(const std::vector<Part>& parts, std::array<float, 3>& min, std::array<float, 3>& max);
}
