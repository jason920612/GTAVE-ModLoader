#pragma once
#include <cstddef>
#include <cstdint>

#include "GTAVBridgeProtocol.h"

// The game's objects for the UE5 renderer (research/phase0.md §60.20): the renderer draws the map as it was placed, so a
// door the player swung open stayed shut, a bin knocked over stood, a buoy did not bob, and what a script made (a
// mission's crates) was not there at all. The game keeps its objects (CObject, 0x690 bytes) in a pool whose pointer it
// stores encrypted (CObject's pool set-up: ~p, rotated right by the key's low five bits + 3, left by 32, xor the key
// rotated left by 30); an object has its model info at +0x20 (the name hash at +0x18 of that), its type at +0x28
// (5), its flags at +0x2C (bit 0: visible), its matrix at +0x60 (right, forward, up, position: rows of four floats) and,
// when the map placed it, the dummy it was made from at +0x3D8 (where the map had it: the dummy's position, +0x90).
namespace loader::graphics::gameobjects
{
	// Finds the pool (once). False when the game's code is not as known.
	bool Install();
	// The objects within `radius` metres of a point that are not as the map placed them (moved, turned, hidden) or not
	// the map's, nearest first, at most `max`; how many.
	size_t Near(const float point[3], float radius, GTAVBridge::GameObject* out, size_t max);
}
