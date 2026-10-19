#pragma once

#define NOMINMAX
#include <Windows.h>

#include <modloader/natives.hpp>

namespace roadster::fan
{
	// Runs the suction fan of the player's Roadster for this frame. strength: pull in g on top of gravity; on: the fan
	// is running (off: only the debug log).
	void Tick(Vehicle v, float strength, bool on, bool debug);
	// Forgets the current car (left it, fan off).
	void Reset();
}
