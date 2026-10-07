#pragma once
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

#include "image.hpp"

// Byte patterns ("48 8B 05 ? ? ? ? 85 C0", as src/loader/pattern.hpp parses them).
namespace relocate
{
	struct Pattern
	{
		std::vector<int> bytes; // -1 = any
		static std::optional<Pattern> Parse(const std::string& text);
		std::string Text() const;
	};

	// Matches in the image's code sections (up to `limit`).
	std::vector<uint32_t> Find(const Image& image, const Pattern& pattern, size_t limit = 16);

	// The shortest pattern starting at `rva` that matches only there: whole instructions, with addresses (rip-relative
	// displacements, branch targets, address immediates) left out. Empty when none up to `maxBytes`.
	std::string MakeSignature(const Image& image, uint32_t rva, size_t maxBytes = 96);
}
