#pragma once
#include <cstdint>

// Region-relative road network (world builder, research/phase0.md §61): path nodes and junctions keep their x/y as
// int16 in 0.25 m units, which only reaches +-8192 m. With these patches every instruction that reads a node's or
// junction's x/y adds the origin of the record's region (its 16-bit area id) from a table, so a region's records can
// hold positions relative to wherever the region lies. Regions with origin 0 read exactly as before.
namespace loader::game::pathregions
{
	// Applies the stubs (tools/pathpatch's output for this game build) when loader.json "worldPathRegions" is on.
	// Every patched byte is checked against the build first: on any difference nothing is patched.
	bool Install();

	bool Installed();

	// The origin of an area id's records, in 0.25 m units (applies from the next read).
	void SetOrigin(uint16_t area, int32_t x, int32_t y);
}
