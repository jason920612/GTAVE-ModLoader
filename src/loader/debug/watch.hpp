#pragma once
#include <cstdint>
#include <vector>

namespace loader::debug
{
	struct Watch
	{
		uintptr_t rva;  // address inside GTA5_Enhanced.exe
		uint8_t size;   // 1, 2, 4 or 8 (must be aligned)
	};

	// Research aid: arms hardware write breakpoints (up to 4) on every thread of the game and
	// logs the writing instruction plus return addresses found on the stack.
	void ArmWriteWatches(const std::vector<Watch>& watches);
}
