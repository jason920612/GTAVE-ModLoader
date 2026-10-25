#pragma once
#include <array>
#include <cstdint>
#include <string>
#include <vector>

#include "convert/resource.hpp"

// The pedestrians' navigation mesh files (navmesh[x][y].ynv, research/phase0.md §61.2, §61.8): one per 150 m square of
// the grid that starts at (-6000, -6000), x and y in sectors of 50 m (so in steps of 3); its index is y/3 * W + x/3
// (W = 100). A resource of 16 KB pages (version 2, no physical block).
//   root: +0x08 page map, +0x10 flags (bit 1: has special links), +0x20 a matrix (identity), +0x60 the size (150, 150,
//   height), +0x70 / +0x80 / +0x88 / +0x118 the vertices, vertex indices, edges, polygons (split arrays: chunks of
//   16 KB), +0x90 the index count, +0x94 / +0x98 the adjacent navmeshes (self, none, then the neighbours that exist:
//   S, W, E, N), +0x120 the quadtree, +0x128 / +0x130 the special links and their index list, +0x138 vertex count,
//   +0x13C polygon count, +0x140 own index, +0x144 data size, +0x148 cover points, +0x14C links, +0x150 link indices.
//   A vertex: x, y, z as u16 over the navmesh's box (its minimum: the quadtree root's). A polygon (0x30 bytes): +0x02
//   vertex count << 5 | flags, +0x04 first index, +0x06 own navmesh index, +0x18 its box (int16, 0.25 m: x0 x1 y0 y1
//   z0 z1). An edge (8 bytes, one per polygon corner): two adjacency words, poly << 5 | slot in the adjacency table
//   (0 self, 1 none) and flags above bit 20; 0x000FFFE1 for none. The quadtree splits a square with 32 polygons or
//   more, down to 37.5 m; a leaf lists the polygons touching it (ascending) and its cover points (8 bytes each).
namespace worldgen::ynv
{
	struct Vertex
	{
		uint16_t x, y, z;
	};

	struct Edge
	{
		uint32_t a, b;
	};

	struct Poly
	{
		uint8_t raw[0x30];
		uint32_t VertexCount() const;
		uint16_t FirstIndex() const;
		uint16_t Navmesh() const;
		void SetVertexCount(uint32_t n);
		void SetFirstIndex(uint16_t v);
		void SetNavmesh(uint16_t v);
	};

	struct Node
	{
		std::array<uint8_t, 16> min{}, max{}; // vectors (x, y, z, and the tag the game keeps in w)
		std::array<int16_t, 6> box{};
		bool leaf = false;
		uint32_t firstCover = 0;
		std::vector<uint16_t> polys;
		std::vector<std::array<uint8_t, 8>> cover;
		std::array<int, 4> children{-1, -1, -1, -1}; // indices into Navmesh::nodes
	};

	struct Navmesh
	{
		uint32_t flags = 1;
		uint32_t format = 0x10011;
		std::array<uint8_t, 0x40> matrix{};
		std::array<uint8_t, 16> size{};
		std::vector<uint32_t> adjacent; // self, 0x3FFF, then the neighbours
		std::vector<Vertex> vertices;
		std::vector<uint16_t> indices;
		std::vector<Edge> edges; // one per index
		std::vector<Poly> polys;
		std::vector<Node> nodes; // the quadtree, root first
		std::vector<std::array<uint8_t, 0x1C>> links;
		std::vector<uint16_t> linkIndices;
		uint32_t index = 0;
		uint32_t dataSize = 0;
		uint32_t tag160 = 0x85CB3561;
	};

	bool Read(const loader::convert::Bytes& file, Navmesh& out, std::string& error);
	bool Write(const Navmesh& navmesh, loader::convert::Bytes& out, std::string& error);
	// The same content (everything the files carry; not where it is): "" or what differs.
	std::string Compare(const Navmesh& a, const Navmesh& b);
}
