#pragma once
#include <cstdint>

// Story-mode state the mods' high-level API is built on (research/phase0.md §34). Game thread, script context.
namespace loader::game::story
{
	struct Changes
	{
		bool character = false; // the player became another story character (or none)
		bool saved = false;     // a story save file was written
		bool loading = false;   // a loading screen began
	};
	// Call once per frame while story mode runs.
	Changes Update();

	// 0 Michael, 1 Franklin, 2 Trevor, -1 none (as of the last Update).
	int32_t Character();

	// Story mode's own autosave request (Global 102550 +10, what e.g. appinternet @27069 does): the autosave_controller
	// script saves when it can, unless autosave is off. False when a request is already waiting.
	bool RequestAutosave();
}
