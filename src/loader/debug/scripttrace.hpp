#pragma once

// Research aid: records which natives a game script calls, and from where (research/phase0.md §28).
// ModLoader\debug_trace.txt names the scripts (one per line); "-<hash>" lines leave a native alone, "+<hash>" lines
// trace only the natives listed so. Every native table entry of those programs is replaced
// with a stub that records (instruction address, native index) in a ring buffer and calls the game's handler. When
// ModLoader\trace_dump.txt appears, the buffer is written to ModLoader\trace.log ("ip index" per line, oldest first;
// the index is the position in the program's native table, as tools/ysc_dis.py numbers them) and cleared.
namespace loader::debug::scripttrace
{
	// Reads debug_trace.txt (once). Returns whether anything is traced.
	bool Load();
	// Patches newly loaded programs and handles the dump trigger. Game thread, once per frame.
	void Tick();
}
