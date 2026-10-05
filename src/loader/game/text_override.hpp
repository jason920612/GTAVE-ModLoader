#pragma once
#include <cstdint>
#include <string_view>

// The game's text store checks a hash map of label -> string before its GXT2 tables
// (TheText + 0x270). Adding entries there defines new labels without touching game text.
namespace loader::game::text_override
{
	bool Init();  // resolves TheText; call once the game code is decrypted
	bool Ready(); // the map exists (it is created while the game loads its text)
	// Adds or replaces a label. The string is copied and kept alive for the session.
	bool Set(uint32_t hash, std::string_view utf8);
}
