#pragma once
#include <cstdint>
#include <map>
#include <vector>

// The script VM's array bounds failure (research/char4-design.md §3.3). The six ARRAY handlers (ARRAY_U8, _LOAD,
// _STORE and the U16 forms) compare the index with the array's count and jump to one shared block, which records the
// instruction and kills the thread. That block is where the fourth character's element is served: for the scripts
// rewritten for the officer (game/char4), index 3 of a three-element array (the per-character arrays) is redirected to
// a shadow element, and the handler goes on as if the array had a fourth element. Other failures are logged and run
// the game's own code (the thread dies, as in the game). Development: ModLoader\debug_arrayfault.txt lines
// "serve <script>" serve the element just past the end of any array for that script (experiment U7), "shadow <global>
// <value> ..." gives such a shadow its first values; with the file, access violations are logged too.
namespace loader::game::arrayfault
{
	// Puts the hook on the failure block. After ResolvePointers and scripts::Install.
	void Install();
	// True when the hook is in place and the six handlers are known (serving works).
	bool Serving();
	// Writes the failures recorded since the last call to the log. Game thread, once per frame.
	void Tick();

	// The programs (name hashes) whose index-3 accesses of three-element arrays are served.
	void ServeFourth(const std::vector<uint32_t>& programs);
	// Shadow elements of global arrays, by the global index of the array (its count slot); made (zeros) on first use.
	// Game thread.
	std::map<uint32_t, std::vector<int64_t>>& Shadows();
	// The game made its global blocks again (a save is loading): shadows are matched to arrays by address again.
	void ForgetAddresses();
	// The fourth character is the player: other array failures of the rewritten scripts get a zeroed scratch element
	// (logged once per place) instead of ending the thread.
	void SetFourthActive(bool active);
}
