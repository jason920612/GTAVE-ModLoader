#pragma once

// Research aid (research/phase0.md §42): with ModLoader\debug_skelhook.txt (an RVA of the per-ped post-animation update)
// present at start, hooks it; ModLoader\debug_skelbend.txt ("<skeleton hex> <bone index> <degrees>") then bends that
// bone after the update and recomputes the object matrices below it, logging an FK check first.
namespace loader::debug::skelhook
{
	void StartIfRequested();
	void Poll(); // game thread
}
