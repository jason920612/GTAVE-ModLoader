#pragma once

// Fixes for bugs of the game itself (research/phase0.md §24).
namespace loader::game::fixes
{
	// Loading screen deadlock: the loading screen thread holds the renderer lock while it waits for a GPU fence
	// that only advances once the main thread submits its frame, and the main thread waits for that lock. The
	// game's watchdog then crashes the game ~60 s later (about 1 start in 6 on this machine, with or without
	// mods). When the main thread has stalled, the loading screen thread stops waiting so the lock is released.
	bool InstallLoadingScreenDeadlockFix();
}
