#pragma once
#include <cstdint>

// Story-mode pause menu: turns the "Online" header tab into a "Mods" tab that lists every mod
// with settings as a category, each with its own page of native toggles/sliders.
// See research/phase0.md §8–10.
namespace loader::game::pausemenu
{
	// Hooks the menu data loader, the column builder and the menu event handler.
	bool InstallHooks();

	// Game thread, every tick. Builds the Mods tab once the menu data and the mods are loaded
	// (always before the pause menu is first opened). Returns true while the tab shows mod
	// settings; false means the caller should keep showing its status text instead.
	bool Tick();
}
