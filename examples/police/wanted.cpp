// The realistic police when the player is the criminal (the police mod's second half, the user's plan: "more
// realistic police when you are the criminal", on the same police core; research/phase0.md §57). Whoever is played
// with a wanted level (a protagonist, or the officer off duty) faces the game's police, who now act on what the player
// does rather than on the stars alone:
// - Force by threat: someone wanted but not armed is arrested, not shot (officers on foot come to arrest, nobody
//   shoots from cars); someone with a gun in hand is held at gunpoint and told to drop it; whoever shoots, aims at or
//   hurts the police (a minute and a half ago or less) gets the game's full response.
// - Dispatch in proportion: the helicopter from three stars, the SWAT from four, the army never.
// - Surrender: a key (H) puts the player's hands up where they stand (unhurt meanwhile, the wanted level down to one
//   star); one officer comes and arrests them (the game's own arrest: busted), the others keep their guns on them;
//   moving away calls it off. Officers hold their fire while the player is no threat (shoot rate 0).
// A setting turns it off. Not while the officer is played on duty (then the police are their colleagues), nor during the
// story's missions.
#include "police.hpp"

#include <algorithm>
#include <atomic>
#include <map>
#include <string>
#include <vector>

namespace police::wanted
{
	namespace
	{
		using ai::Distance;

		ml::Setting g_enabled;
		std::atomic<bool> g_surrender = false;
		bool g_active = false;
		int g_stars = -1;
		uint64_t g_aggressionAt = 0; // the player last shot at, aimed at or hurt the police
		uint64_t g_nextScan = 0, g_nextWord = 0;
		std::map<::Ped, uint64_t> g_tasked; // officers given an arrest / hold task, and when
		int g_threat = -1;

		constexpr int kPoliceAuto = 1, kPoliceHeli = 2, kSwatAuto = 4, kSwatHeli = 12, kArmy = 14;

		bool Officer(::Ped p)
		{
			if (!p || !ENTITY::DOES_ENTITY_EXIST(p) || ENTITY::IS_ENTITY_DEAD(p, false) || PED::IS_PED_A_PLAYER(p))
				return false;
			const int type = PED::GET_PED_TYPE(p);
			// The game's police (cops, SWAT), not the mod's own people (backup, colleagues: mission entities).
			return (type == 6 || type == 27) && !ENTITY::IS_ENTITY_A_MISSION_ENTITY(p);
		}

		void Dispatch(int stars)
		{
			MISC::ENABLE_DISPATCH_SERVICE(kPoliceHeli, stars >= 3);
			MISC::ENABLE_DISPATCH_SERVICE(kSwatAuto, stars >= 4);
			MISC::ENABLE_DISPATCH_SERVICE(kSwatHeli, stars >= 4);
			MISC::ENABLE_DISPATCH_SERVICE(kArmy, false);
			MISC::ENABLE_DISPATCH_SERVICE(kPoliceAuto, true);
		}

		void Vanilla()
		{
			for (const int s : {kPoliceAuto, kPoliceHeli, kSwatAuto, kSwatHeli, kArmy})
				MISC::ENABLE_DISPATCH_SERVICE(s, true);
		}

		std::vector<::Ped> g_held; // officers told not to fire (shoot rate 0)
		// Surrendered: hands up where they stood; one officer comes to arrest (the game's own arrest: busted), the others
		// keep their guns on them. Moving away calls it off.
		bool g_surrendered = false;
		Vector3 g_surrenderAt{};
		::Ped g_arrester = 0;
		uint64_t g_arrestTold = 0;

		void EndSurrender(::Ped pl)
		{
			if (!g_surrendered)
				return;
			g_surrendered = false;
			g_arrester = 0;
			if (ENTITY::DOES_ENTITY_EXIST(pl))
				ENTITY::SET_ENTITY_INVINCIBLE(pl, false, false);
		}

		// The nearest officer on foot arrests; the rest hold their guns on the player.
		void ArrestBy(::Ped pl, const std::vector<::Ped>& officers)
		{
			const Vector3 me = ENTITY::GET_ENTITY_COORDS(pl, true);
			::Ped nearest = 0;
			float best = 120.0f;
			for (const ::Ped p : officers)
				if (const float d = Distance(ENTITY::GET_ENTITY_COORDS(p, true), me); d < best && !PED::IS_PED_IN_ANY_VEHICLE(p, false))
					best = d, nearest = p;
			if (!nearest || nearest == g_arrester)
				return;
			g_arrester = nearest;
			for (const ::Ped p : officers)
			{
				TASK::CLEAR_PED_TASKS(p);
				if (p == nearest)
					TASK::TASK_ARREST_PED(p, pl);
				else if (!PED::IS_PED_IN_ANY_VEHICLE(p, false))
					TASK::TASK_AIM_GUN_AT_ENTITY(p, pl, -1, false);
				g_tasked[p] = ml::TickMs();
			}
			ml::Log("wanted: an officer {:.0f} m off comes to arrest", best);
		}

		void Release()
		{
			for (auto& [p, at] : g_tasked)
				if (ENTITY::DOES_ENTITY_EXIST(p) && !ENTITY::IS_ENTITY_DEAD(p, false))
					TASK::CLEAR_PED_TASKS(p);
			g_tasked.clear();
			for (const ::Ped p : g_held)
				if (ENTITY::DOES_ENTITY_EXIST(p))
				{
					PED::SET_PED_SHOOT_RATE(p, 100);
					PED::SET_PED_COMBAT_ATTRIBUTES(p, 2, true); // drive-bys again
				}
			g_held.clear();
		}

		void Say(const char* line)
		{
			HUD::BEGIN_TEXT_COMMAND_PRINT("STRING");
			HUD::ADD_TEXT_COMPONENT_SUBSTRING_PLAYER_NAME(line);
			HUD::END_TEXT_COMMAND_PRINT(3500, true);
		}

		// How much of a threat the player is now: 0 not armed, 1 a weapon in hand, 2 they went for the police.
		int Threat(::Ped pl, const std::vector<::Ped>& officers)
		{
			const uint64_t now = ml::TickMs();
			const Vector3 me = ENTITY::GET_ENTITY_COORDS(pl, true);
			for (const ::Ped p : officers)
			{
				if (PLAYER::IS_PLAYER_FREE_AIMING_AT_ENTITY(PLAYER::PLAYER_ID(), p) || ENTITY::HAS_ENTITY_BEEN_DAMAGED_BY_ENTITY(p, pl, true))
				{
					ENTITY::CLEAR_ENTITY_LAST_DAMAGE_ENTITY(p);
					g_aggressionAt = now;
				}
				if (PED::IS_PED_SHOOTING(pl) && Distance(ENTITY::GET_ENTITY_COORDS(p, true), me) < 80.0f)
					g_aggressionAt = now;
			}
			if (g_aggressionAt && now - g_aggressionAt < 90000)
				return 2;
			return WEAPON::IS_PED_ARMED(pl, 4 | 2) ? 1 : 0;
		}
	}

	void Build()
	{
		g_enabled = ml::AddToggle("realisticWanted", "寫實警察：被通緝時警察依情況用武力（沒武器就逮捕、不開槍；H 投降）", true);
		ml::Hotkey("hkSurrender", "被通緝時舉手投降", 0x48 /* H */, [] { g_surrender = true; });
	}

	void Forget()
	{
		g_tasked.clear();
		g_held.clear();
		g_surrendered = false;
		g_arrester = 0;
		g_active = false;
		g_stars = -1;
		g_threat = -1;
		g_aggressionAt = 0;
	}

	void Tick()
	{
		const ::Ped pl = PLAYER::PLAYER_PED_ID();
		const int stars = PLAYER::GET_PLAYER_WANTED_LEVEL(PLAYER::PLAYER_ID());
		// (not in the story's missions: they count on the game's police as they are)
		if (g_surrendered && stars == 0)
			EndSurrender(pl); // arrested (or let go)
		const bool active = g_enabled && stars > 0 && !(officer::IsPlayer() && duty::OnDuty()) && !ENTITY::IS_ENTITY_DEAD(pl, false) && !MISC::GET_MISSION_FLAG();
		if (!active)
		{
			g_surrender = false;
			EndSurrender(pl);
			if (g_active)
			{
				Release();
				Vanilla();
				g_active = false;
				g_stars = -1;
				g_threat = -1;
				g_aggressionAt = 0;
				ml::Log("wanted: over");
			}
			return;
		}
		if (!g_active)
		{
			g_active = true;
			ml::Log("wanted: the realistic police take over ({} stars)", stars);
		}
		if (stars != g_stars)
		{
			g_stars = stars;
			Dispatch(stars);
		}

		const uint64_t now = ml::TickMs();
		if (now < g_nextScan && !g_surrender)
			return;
		g_nextScan = now + 500;
		const Vector3 me = ENTITY::GET_ENTITY_COORDS(pl, true);
		std::vector<::Ped> officers;
		int64_t buf[1 + 40] = {40};
		const int n = PED::GET_PED_NEARBY_PEDS(pl, reinterpret_cast<Any*>(buf), -1);
		for (int i = 0; i < n && i < 40; ++i)
			if (const ::Ped p = static_cast<::Ped>(buf[1 + i]); Officer(p))
				officers.push_back(p);

		const int threat = Threat(pl, officers);
		static uint64_t nextLog = 0;
		if (now >= nextLog)
		{
			nextLog = now + 5000;
			int tasked = 0;
			for (const ::Ped p : officers)
				tasked += g_tasked.contains(p);
			ml::Log("wanted: {} stars, threat {}, {} officers near ({} given a task), health {}", stars, threat, officers.size(), tasked,
			    ENTITY::GET_ENTITY_HEALTH(pl));
		}
		if (threat != g_threat)
		{
			if (threat == 2)
			{
				Release(); // the game's full response
				ml::Log("wanted: the player went for the police: full force");
			}
			g_threat = threat;
		}

		// Surrender: hands up where they stand (unhurt meanwhile), the wanted level down to one star (giving up
		// de-escalates), one officer comes to arrest them.
		if (g_surrender.exchange(false) && !PED::IS_PED_IN_ANY_VEHICLE(pl, false) && threat < 2 && !g_surrendered)
		{
			g_surrendered = true;
			g_surrenderAt = me;
			g_arrester = 0;
			WEAPON::SET_CURRENT_PED_WEAPON(pl, MISC::GET_HASH_KEY("WEAPON_UNARMED"), true);
			TASK::CLEAR_PED_TASKS(pl);
			TASK::TASK_HANDS_UP(pl, -1, 0, -1, 0);
			ENTITY::SET_ENTITY_INVINCIBLE(pl, true, false);
			if (stars > 1)
			{
				PLAYER::SET_PLAYER_WANTED_LEVEL(PLAYER::PLAYER_ID(), 1, false);
				PLAYER::SET_PLAYER_WANTED_LEVEL_NOW(PLAYER::PLAYER_ID(), false);
			}
			for (const ::Ped p : officers)
			{
				PED::SET_PED_SHOOT_RATE(p, 0);
				if (std::find(g_held.begin(), g_held.end(), p) == g_held.end())
					g_held.push_back(p);
			}
			ArrestBy(pl, officers);
			Say("你舉手投降了：別動，等警察過來");
			ml::Log("wanted: the player surrendered ({} officers near)", officers.size());
			return;
		}
		if (g_surrendered)
		{
			if (PLAYER::IS_PLAYER_BEING_ARRESTED(PLAYER::PLAYER_ID(), true))
				return; // (cuffed and led away: the game's arrest)
			if (Distance(me, g_surrenderAt) > 3.0f || threat == 2)
			{
				EndSurrender(pl);
				Say("你逃跑了");
				ml::Log("wanted: the surrender called off");
			}
			else
			{
				for (const ::Ped p : officers)
					PED::SET_PED_SHOOT_RATE(p, 0);
				if (!g_arrester || !ENTITY::DOES_ENTITY_EXIST(g_arrester) || ENTITY::IS_ENTITY_DEAD(g_arrester, false) ||
				    TASK::GET_SCRIPT_TASK_STATUS(g_arrester, MISC::GET_HASH_KEY("SCRIPT_TASK_ARREST_PED")) == 7)
					ArrestBy(pl, officers);
				if (now >= g_arrestTold)
				{
					g_arrestTold = now + 8000;
					Say("警察：慢慢來，手放頭上，不要動！");
				}
				return;
			}
		}
		if (threat == 2)
			return;

		// Not armed: arrested, not shot. Armed: held at gunpoint, not shot until they go for the police. Nobody shoots
		// from cars.
		const ::Ped* close = nullptr;
		for (const ::Ped& p : officers)
		{
			PED::SET_PED_COMBAT_ATTRIBUTES(p, 2, false); // no drive-bys
			PED::SET_PED_SHOOT_RATE(p, 0);
			if (std::find(g_held.begin(), g_held.end(), p) == g_held.end())
				g_held.push_back(p);
			const float d = Distance(ENTITY::GET_ENTITY_COORDS(p, true), me);
			if (PED::IS_PED_IN_ANY_VEHICLE(p, false))
			{
				if (VEHICLE::GET_PED_IN_VEHICLE_SEAT(PED::GET_VEHICLE_PED_IS_IN(p, false), -1, false) == p)
					PED::SET_DRIVER_AGGRESSIVENESS(p, stars <= 1 ? 0.25f : stars == 2 ? 0.5f : 0.8f); // no ramming for a minor offence
				continue;
			}
			if (d > (threat == 0 ? 70.0f : 45.0f))
				continue;
			const auto it = g_tasked.find(p);
			const Hash task = MISC::GET_HASH_KEY(threat == 0 ? "SCRIPT_TASK_ARREST_PED" : "SCRIPT_TASK_AIM_GUN_AT_ENTITY");
			if (it == g_tasked.end() || (now - it->second > 8000 && TASK::GET_SCRIPT_TASK_STATUS(p, task) == 7))
			{
				if (threat == 0)
					TASK::TASK_ARREST_PED(p, pl);
				else
					TASK::TASK_AIM_GUN_AT_ENTITY(p, pl, -1, false);
				g_tasked[p] = now;
			}
			if (d < 25.0f && (!close || d < Distance(ENTITY::GET_ENTITY_COORDS(*close, true), me)))
				close = &p;
		}
		if (close && now >= g_nextWord)
		{
			g_nextWord = now + 9000;
			Say(threat == 0 ? "警察：警察！不許動！雙手舉起來！（H 投降）" : "警察：放下武器！馬上放下！");
		}
		// (officers no longer around)
		std::erase_if(g_tasked, [](const auto& t) { return !ENTITY::DOES_ENTITY_EXIST(t.first) || ENTITY::IS_ENTITY_DEAD(t.first, false); });
	}
}
