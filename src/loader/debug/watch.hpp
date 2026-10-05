#pragma once
#include <cstdint>
#include <vector>

namespace loader::debug
{
	struct Watch
	{
		uintptr_t rva;  // address inside GTA5_Enhanced.exe (or absolute when absolute = true)
		uint8_t size;   // 1, 2, 4 or 8 (must be aligned)
		bool onRead = false; // break on read or write instead of write only
		bool absolute = false;
	};

	// Research aid: arms hardware write breakpoints (up to 4) on every thread of the game and
	// logs the writing instruction plus return addresses found on the stack.
	void ArmWriteWatches(const std::vector<Watch>& watches);

	// Research aid: if ModLoader\debug_watch.txt changed, arm the watches it lists
	// (one per line: <hex address> <size> [r]). Cheap when the file does not exist.
	void PollWatchFile();

	// Writes recorded hits to the log (call from a normal thread, e.g. the game tick).
	void FlushHits();
}
