#pragma once

// The game's map of the world builder's new land (research/phase0.md §61.26). A land's pack brings it in the game's own
// style (worldgen's minimap.cpp) and says what it has in land.json ("map": "name"; "radar": [column, row] of each supertile
// on the game's radar lattice, its file minimap_<column + 1000>_<row + 1000>.ydd; "pictures": [row, column] of each
// square on the pause map's picture lattice, minimap_<name>_<row>_<column>.ytd and minimap_sea_<name>_...).
//  - The radar: the game streams and draws a fixed 8 x 9 array of supertiles (minimap_0_0..7_8). The new land's are
//    entries of the same kind beside it, wanted for the same view (the game's own selection, unclamped), streamed and
//    drawn by the game's own functions with the game's.
//  - The pause map: its pictures are a grid minimap.ymt sizes (2 x 3 squares of 4500 m); at the session's start it is
//    widened to take in the new land's squares, each square's dictionary then named after whoever has a picture of it:
//    a land, the game, or a land's open sea.
namespace loader::game::minimap
{
	// Reads the lands of the enabled packs and hooks the radar and the pause map (each patched call checked against the
	// build first: on any difference nothing is patched). Once the game is unpacked.
	bool Install();
}
