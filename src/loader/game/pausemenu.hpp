#pragma once
#include <cstdint>

namespace loader::game::pausemenu
{
	// Hooks the pause menu data loader so screens/items can be added right after
	// pausemenu.xml is parsed, before the menu builds its runtime state from it.
	bool InstallHooks();

	// Game thread, every tick: text entries for our labels, preference changes.
	void Tick();
}
