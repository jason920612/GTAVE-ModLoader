#pragma once
#include <cstdint>
#include <set>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

#include "program.hpp"

// The rewrite that makes a game script see the fourth character (research/char4-design.md §3.2-3.4; tested by hand in
// M0 with tools/char4_identity.py and tools/char4_triples.py). Scripts keep their vanilla behaviour while the fourth
// character is not the player: every change asks the officer native first.
//   - Identity: get_cur (the current character, from update_cur) and is_playable (c < 3) are joined by get_cur4 and
//     is_char4, which also answer 3 / true for the officer; every call is pointed at them, except the calls inside
//     update_cur and the functions it uses (the saved current character never becomes 3). Inline "c == 0 || c == 1 ||
//     c == 2" chains become an is_char4 call.
//   - Character switches: a SWITCH whose cases 0, 1 and 2 differ only in a pushed per-character stat hash (SP0_X /
//     SP1_X / SP2_X) or in the pushed character itself gets a case 3 (SP3_X, or 3), copied from case 2. Loose form
//     (MONEY_TOTAL_SPENT): Michael's case does more, the case is copied from Trevor's. The SWITCH table cannot grow in
//     place, so the function is re-emitted at the end of the code with the new cases and its old body forwards to it.
//   - Accounts: the payment wrappers refuse account 3 (originally "no account"; the officer's account is 3): only 4
//     is refused now.
//   - Script rules: gunclub_shop never offers the officer heavy weapons, explosives or incendiaries (§0.0 Q9, kForbiddenWeapons).
namespace loader::char4
{
	// The native the rewritten scripts call for "the officer is the player": GET_BENCHMARK_PASS (game hash; its handler
	// is not shared and none of these scripts call it). The loader answers it for them (game/char4).
	constexpr uint64_t kOfficerNative = 0x7007C113A8466ADEULL;

	// Weapons Ammu-Nation never offers the fourth character (the police mod's rule, §0.0 Q9): heavy weapons,
	// explosives and incendiaries, ammunition included. The shop's unlock check (character, weapon) and its
	// always-unlocked check (weapon; DLC weapons whose content is not locked) refuse them first.
	inline constexpr const char* kForbiddenWeapons[] = {
	    // heavy weapons
	    "WEAPON_RPG", "WEAPON_GRENADELAUNCHER", "WEAPON_GRENADELAUNCHER_SMOKE", "WEAPON_MINIGUN", "WEAPON_FIREWORK", "WEAPON_RAILGUN",
	    "WEAPON_HOMINGLAUNCHER", "WEAPON_COMPACTLAUNCHER", "WEAPON_EMPLAUNCHER", "WEAPON_RAYMINIGUN", "WEAPON_RAILGUNXM3",
	    // explosives and incendiaries
	    "WEAPON_GRENADE", "WEAPON_STICKYBOMB", "WEAPON_MOLOTOV", "WEAPON_PROXMINE", "WEAPON_PIPEBOMB"};

	// Per-character stats of the game (spstatssetup.xml, characterStat="true"): the name without its SPn_ prefix and
	// the type, by the hash of each character's name.
	struct CharacterStats
	{
		struct Stat
		{
			std::string name, type;
		};
		std::vector<Stat> stats;
		std::unordered_map<uint32_t, uint32_t> byHash[3]; // SPk_<name> hash -> index into stats
		void Parse(std::string_view xml);
		const Stat* Find(int character, uint32_t hash) const;
	};

	struct Report
	{
		int curCalls = 0, playableCalls = 0, chains = 0;
		int statSwitches = 0, looseSwitches = 0, identitySwitches = 0, moved = 0, accountChecks = 0;
		std::set<std::string> stats; // the SP3_ stats the script now uses (names without the prefix)
		std::vector<std::string> notes; // switches on 0/1/2 that were left alone, and similar
	};

	// Rewrites the program. False (with `error`) when the identity helpers are not found or a change cannot be laid out:
	// the script must then stay vanilla.
	bool Rewrite(Program& p, const CharacterStats& stats, Report& report, std::string& error);

	// gunclub_shop's weapon unlock bits (weapon -> index; stat WEAP_UNLOCK_<index / 32>, bit index % 32): the bits of the
	// forbidden weapons in WEAP_UNLOCK_0..3. Empty when the table is not found.
	std::vector<uint32_t> ForbiddenUnlockBits(const Program& vanillaGunclub);
}
