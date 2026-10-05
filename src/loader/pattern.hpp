#pragma once
#include <cstdint>
#include <optional>
#include <span>
#include <string_view>
#include <vector>

namespace loader::pattern
{
	// IDA-style pattern, e.g. "48 8B 05 ? ? ? ? 48 89". '?' or '??' is a wildcard.
	struct Pattern
	{
		std::vector<int16_t> bytes; // -1 = wildcard

		static Pattern Parse(std::string_view text);
	};

	struct Module
	{
		uintptr_t base = 0;
		size_t size = 0;
		std::span<const uint8_t> text; // first executable section (the game's real code)

		static Module Main();
	};

	std::optional<uintptr_t> Find(std::span<const uint8_t> region, const Pattern& pattern);
	// Number of matches, stops counting at `limit`.
	size_t Count(std::span<const uint8_t> region, const Pattern& pattern, size_t limit = 2);

	// Resolves a rip-relative operand: `at` points to the 32-bit displacement.
	inline uintptr_t Rip(uintptr_t at)
	{
		return at + 4 + *reinterpret_cast<const int32_t*>(at);
	}
}
