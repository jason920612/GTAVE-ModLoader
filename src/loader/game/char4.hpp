#pragma once
#include <cstdint>
#include <string>
#include <vector>

#include "../convert/archive.hpp"

// The fourth story character (research/char4-design.md; phase0.md §55): a mod's own player character (the police
// mod's officer) that the game's shops, ATMs, services and activities treat as character 3, with money of its own.
//   - Scripts: the loader's pack carries rewritten copies of those scripts (char4/generate.hpp); they ask "is the
//     officer the player" through an appended native, which the loader answers for them.
//   - Data: their index-3 accesses of per-character arrays are served from shadow elements (arrayfault), kept per
//     game save (ModLoader\core\char4\saves\<save number>.json).
//   - Money: the officer's stats are SP3_* (declared by the pack); while the officer is the player the engine's stats
//     slot 4 is pointed at them (the HUD cash, pickups, the cash change of a respray), the account of character 3 is
//     3, and the scripts' account cash cache for 3 follows SP3_TOTAL_CASH.
// A mod names the character's ped (SDK Char4SetPed); the loader follows whether it is the player every frame.
namespace loader::game::char4
{
	// The files in the loader's pack. DLC list time (dlcpacks), with the game's archive decryption.
	void Prepare(const convert::Decryptor* decrypt);
	// Natives are resolved: the officer native is answered for the rewritten scripts. Game thread, first tick.
	void Install();
	// Game thread, script context, every frame (before the mods).
	void Tick();

	// SDK: the fourth character's ped (0 = none).
	void SetPed(int32_t ped);
	// The rewritten scripts are in the game and served: the fourth character works.
	bool Ready();
	// The fourth character is the player now (as of the last Tick).
	bool IsPlayer();

	// The game saved with this save number (0 = none): the shadows are written for it. Game thread.
	void Saved(int32_t nonce);
	// A save is loading: shadows are matched by address again, and the loaded save's are read once it is in.
	void Loading();
	void Loaded(int32_t nonce);
	// GTA Online: everything back to the game's own.
	void Suspend();
}
