#pragma once
#include <cstdint>

// Streaming store sizes. The game sizes each asset store (texture dictionaries, fragments, ped metadata, ...)
// from its pool configuration (gameconfig) when the store is created; add-on content can fill a store, and the
// game then asserts while registering files. Stores used by add-on packs get extra room (research/phase0.md §18).
namespace loader::game::pools
{
	// Hooks store creation. Call once the game is decrypted, before the stores are created.
	bool InstallHooks();

	// A store that needs at least this many entries (whatever its configuration says). Before the stores are created.
	void SetMinimum(const char* name, uint32_t size);
}
