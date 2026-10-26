#pragma once

// The player's play area for the world builder (research/phase0.md §61.27). The game keeps the player within
// (-8000, -8000, -1700)..(8000, 9000, 2700): its player update (0xD7F1E0) takes a player outside that box as having left
// the map (0xD855C0: their vehicle held where it is, the position clamped back), so a car the player is in - at the
// wheel or not, driven by them or by the AI - does not move there; two more checks (0x9F5F50, 0x1A653C0) treat the
// player as off the map too. The box is built once (on first use) from tunables (0x434A2EC: max x, max y, min x, min y)
// into 0x43524A0 (min) and 0x43524B0 (max), flag 0x43524C0; the game's start (0xDAE890) zeroes the box but not the flag,
// so the box cannot be set ahead of it. With new land beyond the game's map (loader.json "worldPathGrid", and
// "worldPlayArea") the tunables (they come with the game's data, after the loader is in) and the box, whenever they hold
// the game's values, get the engine's world box in x and y (+-16000).
namespace loader::game::playarea
{
	// Checks the build; Tick (the game's thread, each frame) does the rest.
	bool Install();
	void Tick();
}
