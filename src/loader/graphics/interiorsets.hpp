#pragma once
#include <cstddef>
#include <cstdint>

#include "GTAVBridgeProtocol.h"

// The game's interiors and the entity sets on in each, for the UE5 renderer (research/phase0.md §60.8): scripts
// switch an interior's entity sets (ACTIVATE_INTERIOR_ENTITY_SET: the rooms of Michael's house as the story goes, none
// of its 17 on by default); the renderer, which knows only the defaults, showed his living room without its furniture.
// The game keeps its interiors' proxies in a pool (its pointer stored encrypted: the code decodes it each time, as
// IS_INTERIOR_ENTITY_SET_ACTIVE does); a proxy has its position at +0x60, its name hash at +0xD4 and the entity sets
// on as 12-byte entries at +0xC0 (the name hash first), their count at +0xC8.
namespace loader::graphics::interiorsets
{
	// Finds the pool (once). False when the game's code is not as known.
	bool Install();
	// The interiors within `radius` metres (across) of a point, nearest first, at most `max`; how many.
	size_t Near(const float point[3], float radius, GTAVBridge::InteriorSets* out, size_t max);
}
