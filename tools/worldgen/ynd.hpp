#pragma once
#include <cstdint>
#include <string>
#include <vector>

#include "convert/resource.hpp"

// The road network's region files (nodes<N>.ynd, research/phase0.md §61): N = area id = cell y * 32 + cell x
// (Cayo Perico's set replaces cells as 1024 + N). A node is 0x28 bytes: area id +0x10, its index +0x12, street name
// hash +0x14, first link +0x1a, x/y +0x1c/+0x1e (int16, 0.25 m), z +0x22 (int16, 1/32 m), link count in +0x25 bits 3-7;
// vehicle nodes first, then pedestrian nodes, each sorted by y. A link is 8 bytes (target area id, node index, 4 bytes
// of lanes and flags), a junction 12 (max z, x, y, min z, offset of its height map, its width and height in cells),
// a junction ref 8 (area id, node index, junction index, unused).
namespace worldgen::ynd
{
	struct Node
	{
		uint8_t raw[0x28];
		uint16_t Area() const;
		uint16_t Index() const;
		uint16_t FirstLink() const;
		int16_t X() const;
		int16_t Y() const;
		int16_t Z() const; // (1/32 m)
		uint32_t LinkCount() const;
		void SetArea(uint16_t v);
		void SetIndex(uint16_t v);
		void SetFirstLink(uint16_t v);
	};

	struct Link
	{
		uint16_t area, node;
		uint8_t flags[4];
	};

	struct Junction
	{
		int16_t maxZ, x, y, minZ;
		uint16_t heightmap;
		uint8_t width, height;
	};

	struct JunctionRef
	{
		uint16_t area, node, junction, unused;
	};

	struct Region
	{
		std::vector<Node> nodes; // vehicle nodes, then pedestrian nodes
		uint32_t vehicleNodes = 0;
		std::vector<Link> links;
		std::vector<Junction> junctions;
		std::vector<uint8_t> heights; // the junctions' height maps
		std::vector<JunctionRef> refs;
		uint32_t unknown48 = 1;
	};

	bool Read(const loader::convert::Bytes& file, Region& out, std::string& error);
	bool Write(const Region& region, loader::convert::Bytes& out, std::string& error);
}
