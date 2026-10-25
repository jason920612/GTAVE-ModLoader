#pragma once
#include <filesystem>
#include <string>

#include "osm.hpp"

// OpenStreetMap's binary extracts (.osm.pbf, the published format: a length-prefixed BlobHeader, then a Blob holding a
// zlib-compressed PrimitiveBlock of nodes - mostly "dense" - ways or relations, protocol buffers throughout), read
// for a box: the nodes in it (and a margin round it), the ways that touch them, the relations of those, the tagged
// nodes' tags. The file must be sorted by type (nodes, then ways, then relations), as the extracts are.
namespace worldgen::pbf
{
	bool Load(const std::filesystem::path& file, double south, double west, double north, double east, double margin, osm::Data& out, std::string& error);
}
