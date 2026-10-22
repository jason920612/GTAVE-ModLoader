#pragma once

// M0 experiment (research/char4-design.md U3/U4): ModLoader\debug_bootscript.txt names a script that is started as soon
// as startup runs at a session start, so that it registers its save data while the loaded save is being applied
// (startup's own registration list is loaded before the replacement packs, so it cannot list the script itself).
namespace loader::debug::bootscript
{
	// Game thread, every tick (story mode or not).
	void Tick();
}
