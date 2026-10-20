#pragma once

// Fixes for bugs of the game itself (research/phase0.md §24).
namespace loader::game::fixes
{
	// Loading screen deadlock: the loading screen thread holds the renderer lock while it waits for a GPU fence
	// that only advances once the main thread submits its frame, and the main thread waits for that lock. The
	// game's watchdog then crashes the game ~60 s later (about 1 start in 6 on this machine, with or without
	// mods). When the main thread has stalled, the loading screen thread stops waiting so the lock is released.
	bool InstallLoadingScreenDeadlockFix();

	// GTA Online vehicles in story mode: shop_controller walks its table of Online content (516 entries, one a frame,
	// every 10 s, @925037) and, for content it takes as not available, pulls the player out of such a vehicle
	// (CLEAR_PED_TASKS_IMMEDIATELY) and deletes the nearest one (@925156). Its availability check (@933608) answers
	// "available" while Global 4521040 is set (only shop_controller reads it), so the loader keeps it set in story
	// mode: Online vehicles (new police cars, add-on shops' cars) can be driven. Research §53.5. Call every frame.
	void KeepOnlineVehicles();
}
