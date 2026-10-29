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

	// The origin of an area id's records, in 0.25 m units (applies from the next read). With the 1024 m grid every region
	// outside +-8192 m gets its south-west corner (the regions inside keep 0: absolute positions).
	void SetOrigin(uint16_t area, int32_t x, int32_t y);

	// New region files: the path store registers each of its 2048 slots that has no file as a placeholder (no file,
	// flag 0x2000000), and the streaming registration refuses a file for a slot that already exists, so a region file
	// for a cell the game has none for (open sea, or anywhere outside the game's map) is never registered and never
	// loads. For the path store only, a placeholder that is neither loaded nor requested counts as a free slot: the file
	// is registered like any new one. Always on (the game's own packs never add region files); research §61.7.
	bool AllowNewRegionFiles();

	// The vehicle AI's choice of the next node at a junction takes a link to a node past its region's count (or to a node
	// of another area) on as a link with no node, and the game exits reading it (research/phase0.md §61.27): such links
	// are passed over, as one into a region not loaded is. ReportGuards logs how many were (from the game's thread).
	bool GuardNextNodeLinks();
	// The route search (vehicles' routes, the GPS) follows links into their regions checking the area but not the node: a
	// link to a node past the region's count, or to a node of another area - a band's region holding another land's nodes
	// while it waits to be unloaded - had it write through what lay there (crash 0xF51346). Such links are passed over.
	bool GuardRouteSearchLinks();
	void ReportGuards();
}
