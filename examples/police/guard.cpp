// Keeps the game's protagonist systems from mistaking the officer for Michael while they are played, until the officer
// is a real fourth character (research/char4-design.md).
//
// With the officer as the player the scripts' current character is 145 (none). Character sheet entry 145 is all zeros,
// and its bank account field 0 is Michael's account. Shops and services look at the character's cash first (0 for 145)
// and refuse (tested: a vending machine; the car wash is not offered at all), but money paid to the player without that
// look (rewards, through the same account field) would reach Michael, and the ATM shows Michael's balance and history
// (tested; the phone's bank page reads the same field). While the officer is played both fields say "no account" (4):
//   - runtime sheet G2349[145].17: read by the payment wrappers (account 3 or 4 = skip). Not while mission_triggerer_d
//     runs: it reads the account without that check (@502327, @506478), and 4 is out of bounds there.
//   - saved sheet G143050[145].17: read by atm_trigger and appinternet. Part of the save, so vanilla again before
//     every save the mod makes and as soon as the officer is not the player. The ATM then opens with an empty page.
// Director Mode is not offered to the officer either: it puts a protagonist back as the player when it ends. main only
// sets the availability when its own reasons change, so its last wish is kept and given back afterwards.
// An arrest: respawn_controller clears the arrested character's stored weapons (PP_INFO WEAPONS[c], @20681-@20800), and
// with character 145 the index is out of bounds: the thread dies with the screen faded out, and the game never comes
// back (tested). While the officer is the player it is told there was no arrest (GET_TIME_SINCE_LAST_ARREST = -1, read
// only by @48025 and @49927), so that part is skipped; the officer loses their weapons as a protagonist would, and pays
// no fee (they have no account yet). That part also holds the walk out of the police station that fades the screen in
// (@28282), so once respawn_controller is done the mod fades in and gives control back.
#include "police.hpp"

#include <algorithm>
#include <cctype>
#include <string_view>

namespace police::guard
{
	namespace
	{
		constexpr uint32_t kSheetAccount145 = 2349 + 1 + 145 * 29 + 17;          // G2349[145].17
		constexpr uint32_t kSavedSheetAccount145 = 143050 + 1 + 145 * 29 + 17;   // G143050[145].17
		constexpr int64_t kMichaelsAccount = 0, kNoAccount = 4;
		constexpr uint64_t kSetDirectorModeAvailable = 0x04655F9D075D0AE5ULL;
		constexpr uint64_t kGetTimeSinceLastArrest = 0x5063F92F07C2A316ULL;

		bool g_mainWantsDirectorMode = true; // main's last SET_DIRECTOR_MODE_AVAILABLE
		bool g_directorModeOff = false;      // the mod took Director Mode away
		bool g_arrested = false;             // the officer is being arrested
		uint64_t g_releaseUntil = 0;         // after an arrest: until then, the screen is faded in once respawn_controller is done
		uint64_t g_releaseFrom = 0;          // ... and not before (main may start respawn_controller a little later)

		void Set(uint32_t index, int64_t value)
		{
			if (int64_t* g = ml::scripts::Global(index); g && *g != value)
				*g = value;
		}

		bool Running(std::string_view name)
		{
			for (const auto& t : ml::scripts::Threads())
				if (std::ranges::equal(t.name, name, [](char a, char b) { return std::tolower(static_cast<unsigned char>(a)) == b; }))
					return true;
			return false;
		}
	}

	void Init()
	{
		ml::scripts::OverrideNative("main", kSetDirectorModeAvailable, [](ml::scripts::NativeCall& call) {
			g_mainWantsDirectorMode = call.Arg<int32_t>(0) != 0;
			if (g_directorModeOff)
				HUD::SET_DIRECTOR_MODE_AVAILABLE(false);
			else
				call.CallOriginal();
		});
		ml::scripts::OverrideNative("respawn_controller", kGetTimeSinceLastArrest, [](ml::scripts::NativeCall& call) {
			if (officer::IsPlayer())
				call.Return<int32_t>(-1);
			else
				call.CallOriginal();
		});
	}

	void Restore()
	{
		Set(kSheetAccount145, kMichaelsAccount);
		Set(kSavedSheetAccount145, kMichaelsAccount);
		if (g_directorModeOff)
		{
			g_directorModeOff = false;
			HUD::SET_DIRECTOR_MODE_AVAILABLE(g_mainWantsDirectorMode);
		}
	}

	void Tick()
	{
		if (!officer::IsPlayer())
		{
			g_arrested = false;
			g_releaseUntil = 0;
			Restore();
			return;
		}
		if (const bool arrested = PLAYER::IS_PLAYER_BEING_ARRESTED(PLAYER::PLAYER_ID(), true); arrested != g_arrested)
		{
			g_arrested = arrested;
			if (arrested)
			{
				WEAPON::REMOVE_ALL_PED_WEAPONS(PLAYER::PLAYER_PED_ID(), true);
				ml::Log("the officer is being arrested: weapons taken");
			}
			else
				g_releaseFrom = ml::TickMs() + 3000, g_releaseUntil = ml::TickMs() + 120000;
		}
		if (g_releaseUntil && ml::TickMs() >= g_releaseFrom && (ml::TickMs() >= g_releaseUntil || !Running("respawn_controller")))
		{
			if (ml::TickMs() < g_releaseUntil && CAMERA::IS_SCREEN_FADED_OUT())
			{
				PLAYER::SET_PLAYER_CONTROL(PLAYER::PLAYER_ID(), true, 0);
				CAMERA::DO_SCREEN_FADE_IN(800);
				ml::Log("the officer is released");
			}
			g_releaseUntil = 0;
		}
		Set(kSheetAccount145, Running("mission_triggerer_d") ? kMichaelsAccount : kNoAccount);
		Set(kSavedSheetAccount145, kNoAccount);
		if (!g_directorModeOff)
		{
			g_directorModeOff = true;
			HUD::SET_DIRECTOR_MODE_AVAILABLE(false);
		}
	}
}
