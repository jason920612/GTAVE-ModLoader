#pragma once

// A wider navigation mesh grid for the world builder (research/phase0.md §61.8): pedestrians' navmeshes are squares
// of 150 m from (-6000, -6000), numbered y * W + x, W and H 100 in the game (x and y up to 9000 m). With these patches
// the grid is 147 x 110 (up to x 16050, y 10500): nav.dat's reader takes the new size, the navmesh stores' name tables
// hold 16384 entries, the dynamic navmeshes (boats and the like) move from 10000.. to 16170.. and the special index from
// 10100 to 16270 (the instructions that carry them, tools/navgrid.py's list), and every navmesh is renumbered as it
// is placed in memory (its own index, the adjacency table, its polygons' and special links' indices), so the game's
// files stay as they are. A navmesh already numbered for the new grid (the world builder's) is left alone.
// The boxes the path server compares in integers (polygons', quadtree nodes', its own queries and dynamic objects') are
// positions x 4 in int16, +-8192 m: they become positions x 2 (+-16384 m). The code that converts to and from them reads
// scaled copies of its constants (research/navscale.tsv), and every navmesh's boxes are worked out again from its
// vertices as it is placed.
namespace loader::game::navgrid
{
	// When loader.json "worldNavGrid" is on. Every patched byte is checked against the build first: on any
	// difference nothing is patched.
	bool Install();
}
