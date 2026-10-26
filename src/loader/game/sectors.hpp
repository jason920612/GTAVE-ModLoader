#pragma once
#include <cstdint>
#include <string>
#include <vector>

// The world builder's sectors (research/phase0.md §62): new lands that share the same place in the game's world (each a
// pack with a land.json, worldgen's), one of them there at a time - the others "dormant": the game does not execute their
// change sets (GROUP_STARTUP: their road network and the game-named navmeshes; GROUP_MAP_SP: their maps). Which one is
// there at the start: loader.json "worldSector" (a pack's folder name), else the first by name. Switch: the old one's
// change sets reverted and the new one's executed in one go on the game thread (the map group's change makes the game
// restart the session, as story mode <-> online), the road network's regions dropped so that they load again from the new
// files, the sea and the map's pictures the new land's. The player waits in Los Santos, the screen faded, until the
// path server has let the old land's navmeshes go (a land's files taken away with one of its navmeshes still loaded
// stops the game), and is put where they were going once the restarted session is in.
namespace loader::game::sectors
{
	// Reads the lands (mods\*\land.json). Before the game executes the packs' change sets.
	void Install();

	// Whether a pack (its folder name, or its name's hash as the content manager has it) is a dormant sector.
	bool Dormant(const std::string& pack);
	bool DormantHash(uint32_t packHash);
	// The sector there now ("" without sectors) and all the lands.
	std::string Active();
	std::vector<std::string> Lands();

	// The game thread: that land there instead of the one that shares its place, and the player then at `at` (x, y; null:
	// wherever the session's restart leaves them). The switch runs on in Tick (the screen faded meanwhile); "" when it
	// started, else what is wrong.
	std::string Switch(const std::string& to, const float* at = nullptr);
	// The game thread, each frame (story mode).
	void Tick();
}
