#pragma once
#include <chrono>
#include <cstdint>
#include <functional>
#include <string>
#include <vector>

// Research: where the game thread's time goes in a call. A thread of ours stops it about every millisecond and unwinds its
// stack: each sample is put down by the innermost game function it is in, and by the return address in the first frame
// inside [from, to) of the game (which of that function's calls it was under). Nothing is allocated while the game thread
// is stopped (it may hold the heap's lock).
namespace loader::debug::profile
{
	// `run` on this (the game's) thread, sampled: "<n> samples; sites <rva>:<count>...; functions <rva>:<count>...", most first.
	std::string Profiled(uintptr_t from, uintptr_t to, const std::function<void()>& run);

	// This (the game's) thread - and the `others` (thread ids) - sampled all along from Start to Stop (each sample: when,
	// the innermost function - of any module - and the game's functions it was called through).
	void Start(const std::vector<unsigned long>& others = {});
	void Stop();

	// Where the thread was between two moments of the sampling: "<n> samples; functions <where>:<count>...; through
	// <rva<rva<rva>:<count>...", most first (<where>: a game function's rva, or <module>+<function's offset>).
	std::string Between(std::chrono::steady_clock::time_point from, std::chrono::steady_clock::time_point to);
}
