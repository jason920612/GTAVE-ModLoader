#pragma once

#include <string>

// The files one thread opens or asks about, and how long that takes it (debug pipe "filewatch"): ntdll's NtCreateFile and
// NtQueryFullAttributesFile hooked while it is on. For the game's thread: a file asked for there waits on the disk, and
// while the disk is busy (the world builder's batch) a frame waited 70-250 ms for one (research/phase0.md §66.7).
namespace loader::debug::filewatch
{
	// Starts watching `thread` (cleared counts).
	bool Start(unsigned long thread);
	// Stops; the files, the most time first: how often, the time in all and the longest.
	std::string Stop();
}
