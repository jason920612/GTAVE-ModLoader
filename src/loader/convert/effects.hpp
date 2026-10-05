#pragma once
#include <cstdint>
#include <unordered_map>
#include <vector>

namespace loader::convert
{
	// Parameter layout of a shader effect (.fxc) as loaded by the game (research/phase0.md §16).
	struct Effect
	{
		std::vector<uint32_t> textures;                    // texture name hashes in slot order
		std::unordered_map<uint32_t, uint32_t> constants;  // legacy ("old") name hash -> name hash
		uint32_t block = 0;                                // bytes of the per-instance parameter block
	};

	// Reads every loaded effect from the game. Effects are loaded before the DLC list is processed.
	// Returns false if the game's effect table was not found.
	bool LoadEffects(std::unordered_map<uint32_t, Effect>& out);
}
