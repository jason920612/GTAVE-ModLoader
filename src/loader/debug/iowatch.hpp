#pragma once
#include <string>

// Research (research/phase0.md §69): how the game opens and reads files - every thread's opens of files whose path has a
// filter in it (NtCreateFile / NtOpenFile: the options - past the file cache or not, overlapped -, the access), and their
// reads (NtReadFile: how many, how much, how big, the time spent in the call; those on the game thread apart).
namespace loader::debug::iowatch
{
	// From now on; `thread` the game thread. False when the hooks could not be made.
	bool Start(const std::wstring& filter, unsigned long thread);
	// Stops and says what was seen.
	std::string Stop();
}
