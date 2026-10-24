#pragma once

namespace GTAVBridge
{
	struct Shared;
}

// The game's scene lights (research/phase0.md §59.12): each frame the game gathers the lights it draws into a list (a
// light's place, colour, reach, cone and its intensity as drawn: after its switching by the hour and by scripts, its
// fades and flicker) and starts a new list at the next frame. A hook on that new start hands the finished list to the
// UE5 renderer, which then lights with the game's own lights rather than guessing them from the map's data.
namespace loader::graphics::gamelights
{
	// Once the game's code is unpacked: the hook (false when that code was not found).
	bool Install();
	// Where the lists go (nullptr: nowhere).
	void SetTarget(GTAVBridge::Shared* shared);
}
