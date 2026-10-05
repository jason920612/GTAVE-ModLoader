#pragma once
#include <cstdint>
#include <string_view>

namespace loader::game::text
{
	// Finds the loaded GXT2 text table that contains `anchorHash`. Slow (scans memory);
	// call from a background thread. Returns false if no table was found.
	bool Locate(uint32_t anchorHash);
	bool Located();

	// Overwrites a text entry in place. The text is cut (on a UTF-8 boundary) to fit the space of
	// the entry as it was when first seen, so neighbouring entries are never touched.
	// Returns false if the label is not in the table or the table moved (call Locate again).
	bool Replace(uint32_t hash, std::string_view utf8);
}
