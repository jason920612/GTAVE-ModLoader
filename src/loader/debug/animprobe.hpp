#pragma once

// Research aid: with ModLoader\debug_animprobe.txt (an RVA) present at start, hooks the animation sequence decoder there
// and logs its output frames and callers (research/phase0.md §42).
namespace loader::debug::animprobe
{
	void StartIfRequested();
}
