// The officer's state in the game's save (ml::save "officer", shared by the three protagonists):
//   { "active": bool (the officer was the player), "x", "y", "z", "heading", "clock": game minutes,
//     "uniform": bool (on duty) }
// It is kept current while the officer is in the world and written whenever the game saves. After a save is loaded the
// game's player is a protagonist; when the officer was the player at that save, they are put back in control where
// they were. ml::save keeps one record for all save slots, so the game clock at the time is recorded too: a loaded
// save whose clock does not match is another save, and the officer stays out of it.
#include "police.hpp"

namespace police::persist
{
	namespace
	{
		bool g_loading = true; // a load (or the game's start) is under way: restore once it is over
		uint64_t g_nextUpdate = 0;

		ml::Json State()
		{
			return ml::save::Get("officer");
		}

		// The game clock in minutes (the date counts: saves are days apart).
		int64_t Clock()
		{
			const int64_t days = CLOCK::GET_CLOCK_YEAR() * 372LL + CLOCK::GET_CLOCK_MONTH() * 31LL + CLOCK::GET_CLOCK_DAY_OF_MONTH();
			return days * 1440 + CLOCK::GET_CLOCK_HOURS() * 60 + CLOCK::GET_CLOCK_MINUTES();
		}
		// A record made at most this many game minutes before the save (a 2 s refresh, a save screen, a fade).
		constexpr int64_t kClockSlack = 30;
	}

	bool Where(Vector3& at, float& heading)
	{
		const ml::Json s = State();
		if (!s["x"].IsNumber())
			return false;
		at = {s["x"].Float(), s["y"].Float(), s["z"].Float()};
		heading = s["heading"].Float();
		return true;
	}

	void Update(bool active)
	{
		const ::Ped ped = officer::Ped();
		if (!ped)
			return;
		const Vector3 at = ENTITY::GET_ENTITY_COORDS(ped, true);
		ml::save::Set("officer", ml::Json{{"active", active}, {"x", at.x}, {"y", at.y}, {"z", at.z}, {"heading", ENTITY::GET_ENTITY_HEADING(ped)},
		    {"clock", Clock()}, {"uniform", officer::InUniform()}});
	}

	void Loading()
	{
		g_loading = true;
		officer::Forget(); // the world was reset: the peds are gone
	}

	void Tick()
	{
		if (g_loading)
		{
			const ::Ped player = PLAYER::PLAYER_PED_ID();
			if (DLC::GET_IS_LOADING_SCREEN_ACTIVE() || !ENTITY::DOES_ENTITY_EXIST(player) || !PLAYER::IS_PLAYER_CONTROL_ON(PLAYER::PLAYER_ID()))
				return;
			g_loading = false;
			look::Loaded();
			const ml::Json s = State();
			Vector3 at{};
			float heading = 0;
			const int64_t gap = Clock() - s["clock"].Int64();
			ml::Log("loaded: officer record active {}, {} game minutes before this save", s["active"].Bool(), gap);
			if (s["active"].Bool() && gap >= -kClockSlack && gap <= kClockSlack && Where(at, heading))
			{
				CAMERA::DO_SCREEN_FADE_OUT(0);
				officer::Restore(at, heading);
				if (s["uniform"].IsBool() && s["uniform"].Bool() != officer::InUniform())
					officer::Dress(s["uniform"].Bool());
				ml::Wait(1500);
				CAMERA::DO_SCREEN_FADE_IN(800);
			}
			return;
		}
		if (ml::TickMs() >= g_nextUpdate)
		{
			g_nextUpdate = ml::TickMs() + 2000;
			Update(officer::IsPlayer());
		}
	}
}
