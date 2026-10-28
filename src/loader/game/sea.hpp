#pragma once
#include <string>

// The sea round the world builder's new land (research/phase0.md §61.20). Out past the game's water grid (water.xml's
// quads in 500 m blocks, x -4000..4500, y -4000..8000) the game takes everywhere for the open sea: its level queries and
// segment tests answer "water at sea level" and it draws the sea there as strips round the grid, under new land too, so
// low streets flood and foam. A land's pack says where its water really is (next to its dlc.rpf, written by worldgen
// build): land.json ("box": min x, min y, max x, max y of its terrain; "waves": the swell round it, 1 the open ocean's;
// "water": its mask) and water.bin (4 m cells over the box, 1: water). In a land's box the queries then answer from the
// mask and the strips leave the box out, its water drawn in its place (rectangles of the mask, coarser further from the
// camera); while the camera is at a land the deep ocean's swell is that land's, easing back to the game's own 1 km out.
namespace loader::game::sea
{
	// Reads the lands of the enabled packs and patches the game's water (each patched byte checked against the build
	// first: on any difference nothing is patched). Once the game is unpacked.
	bool Install();

	// Game thread, story mode, each tick.
	void Tick();

	// Game thread: a land about to be crossed into without a cut (game/sectors) - its water mask read and its rectangles
	// worked out on a thread of their own (60-80 ms of the game thread's when it was first there); then, the frame the land
	// there changes, which lands are there taken again at once (not up to 250 ms later).
	void Prepare(const std::string& land);
	void Refresh();
}
