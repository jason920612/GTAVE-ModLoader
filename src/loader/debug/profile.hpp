#pragma once
#include <cstdint>
#include <functional>
#include <string>

// Research: where the game thread's time goes in a call. A thread of ours stops it about every millisecond and unwinds its
// stack: each sample is put down by the innermost game function it is in, and by the return address in the first frame
// inside [from, to) of the game (which of that function's calls it was under). Nothing is allocated while the game thread
// is stopped (it may hold the heap's lock).
namespace loader::debug::profile
{
	// `run` on this (the game's) thread, sampled: "<n> samples; sites <rva>:<count>...; functions <rva>:<count>...", most first.
	std::string Profiled(uintptr_t from, uintptr_t to, const std::function<void()>& run);
}
