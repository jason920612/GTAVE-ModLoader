#pragma once
#include <string>

// A fatal error held instead of ending the game (development: ModLoader\debug_arrayfault.txt). The game's own fatal checks
// (an int3 in its code) and exceptions nothing handles stop the thread they happen on: the moment is written out (a
// minidump in ModLoader\crash - the threads, their stacks, the modules' data and the memory they point at; the whole memory
// with "dump full" in the file), the game's hang watchdog is held off, and the rest of the game waits with it. The remote
// pipe still answers - peek reads memory as it was at the fault - and decides: "crash resume" lets the game's own handling
// go on (it ends), "crash skip" steps over an int3 (the game's check gone on past), "crash dump full" writes the whole
// memory. A message box tells whoever is at the game.
namespace loader::debug::watchdog
{
	void Install();

	// The pipe's "crash" command: [status] | dump [full] | resume | skip | test (an int3 of the game's own code on the game
	// thread: held, then "crash skip" goes on).
	std::string Command(const std::string& args);

	// Game thread, from the pipe's jobs: the test's int3.
	std::string Test();
}
