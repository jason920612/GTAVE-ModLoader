// The officer as the game's fourth protagonist (the loader's fourth character, research/char4-design.md): while the
// officer is the player, Ammu-Nation, the ATMs, the services and the paid activities take the officer's own money
// (SP3_TOTAL_CASH, the cash the HUD shows) and keep the officer's own records. The loader is told which ped is the
// officer every frame (a new ped after SET_PLAYER_MODEL is named again).
// A new officer (no unlocks yet in this save) gets savings to start with and the weapons the force allows off duty
// (§0.0 Q9): every ordinary gun, melee weapon, armour and attachment, and the non-explosive throwables, from the
// start. The loader's gunclub_shop never offers the officer heavy weapons, explosives or incendiaries.
#include "police.hpp"

namespace police::money
{
	namespace
	{
		constexpr int kStartingCash = 5000;
		bool g_checked = false; // this session's save was looked at
	}

	void Loading()
	{
		g_checked = false;
	}

	void Tick()
	{
		if (!ml::char4::Available())
			return;
		const ::Ped officer = officer::Ped();
		ml::char4::SetPed(officer);
		if (g_checked || !officer)
			return;
		g_checked = true;
		int unlocks = 0;
		if (!STATS::STAT_GET_INT(MISC::GET_HASH_KEY("SP3_WEAP_UNLOCK_0"), &unlocks, -1) || unlocks != 0)
			return;
		for (const char* stat : {"SP3_WEAP_UNLOCK_0", "SP3_WEAP_UNLOCK_1", "SP3_WEAP_ADDON_UNLOCK_0", "SP3_WEAP_ADDON_UNLOCK_1", "SP3_WEAP_ADDON_UNLOCK_2",
		         "SP3_WEAP_ADDON_UNLOCK_3", "SP3_WEAP_ADDON_UNLOCK_4"})
			STATS::STAT_SET_INT(MISC::GET_HASH_KEY(stat), -1, true);
		int cash = 0;
		if (STATS::STAT_GET_INT(MISC::GET_HASH_KEY("SP3_TOTAL_CASH"), &cash, -1) && cash == 0)
			STATS::STAT_SET_INT(MISC::GET_HASH_KEY("SP3_TOTAL_CASH"), kStartingCash, true);
		ml::Log("a new officer: weapons unlocked, ${} to start with", kStartingCash);
	}

	int Cash()
	{
		int cash = 0;
		STATS::STAT_GET_INT(MISC::GET_HASH_KEY("SP3_TOTAL_CASH"), &cash, -1);
		return cash;
	}
}
