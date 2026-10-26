#pragma once
#include <cstdint>
#include <string>

// The game's DLC content manager (research/phase0.md §62): its packs and their change sets - the groups each pack has
// (GROUP_STARTUP, GROUP_MAP_SP, ...) and which of their change sets are executed - and a pack's group executed or reverted
// while the game runs, as story mode <-> online does with the map groups. For the world builder's sectors (a pack each).
namespace loader::game::content
{
	// Checks the build (the manager, its two calls).
	bool Install();
	// The packs (the game's thread): each one's name hash, groups with their change sets, the change sets executed.
	std::string Describe();
	// A pack's group of change sets executed / reverted (the game's thread); false when there is no such pack or group.
	bool Execute(uint32_t pack, uint32_t group);
	bool Revert(uint32_t pack, uint32_t group);
}
