#pragma once
#include <cstdint>
#include <functional>
#include <string>

// Every model the game knows (research/phase0.md §21): the archetype hash map gives hash -> model info, whose
// type byte tells vehicles from peds. Names are not kept in memory, so they come from the file names in the
// game's archives and in the mod packs (<model>.yft / .ydd), read once in the background and cached.
namespace loader::game::models
{
	enum class Type : uint8_t
	{
		Vehicle = 5,
		Ped = 6,
	};

	// Finds the model table. Call once the game is decrypted (after dlcpacks::InstallHooks: names of encrypted
	// archives need its decryption).
	bool Install();

	// Starts reading model names from the archives (background thread).
	void StartNameScan();
	bool NamesReady();

	// Calls `visit(hash, name, pack)` for every registered model of `type`. `name` is empty when unknown,
	// `pack` is the mod pack folder for models from ModLoader\mods, empty for the game's own. Game thread.
	// Returns the number of models, or -1 while the names are still being read / the table was not found.
	int32_t Enumerate(Type type, const std::function<void(uint32_t hash, const std::string& name, const std::string& pack)>& visit);
}
