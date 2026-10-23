// The realistic police when the player is the criminal (the police mod's second half, the user's plan: "more
// realistic police when you are the criminal", on the same police core; research/phase0.md §57). Whoever is played
// with a wanted level (a protagonist, or the officer off duty) faces the game's police, who now act on what the player
// does rather than on the stars alone:
// - Force by threat: someone wanted but not armed is not shot (officers come up with their guns on them and tell them
//   to give up, nobody fires out of a vehicle); someone with a gun in hand is held at gunpoint and told to drop it;
//   whoever shoots, aims at, hurts or kills the police (a minute and a half ago or less) gets the game's full response.
// - Dispatch in proportion: the helicopter from three stars, the SWAT from four, the army never.
// - Surrender: a key (H) puts the player's hands up where they stand (unhurt meanwhile, the wanted level down to one
//   star and kept there); one officer comes and arrests them (the game's own arrest: busted), the others keep their
//   guns on them; moving away calls it off. Possible after going for the police too (giving up is giving up).
// Officers on foot the mod directs ignore the game's events (shots, the player seen): otherwise they drop the task and
// open fire as the stars say (a shoot rate of 0 alone does not stop them). A setting turns it off. Not while the officer
// is played on duty (then the police are their colleagues), nor during the story's missions.
#include "police.hpp"

#include <algorithm>
#include <atomic>
#include <format>
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
		uint64_t g_aggressionAt = 0; // the player last shot at, aimed at, hurt or killed the police
		uint64_t g_firedAt = 0;      // the player last fired (watched every frame: a shot is one frame)
		// The police the character has killed (the game's stats): the kill that made them wanted comes before the
		// police mind them, and the body is not always around (or among the peds nearby).
		int g_copKills = -1, g_killsOf = -1;
		uint64_t g_nextKills = 0;
		uint64_t g_nextScan = 0, g_nextWord = 0;
		// (The game's arrest task fires at anyone wanted at two stars or more: it is left to the arrest of a player who
		// surrendered, at one star.)
		enum class Task
		{
			Arrest,
			Aim,
			Approach, // to the player, gun on them, not firing
			Seated,   // in a vehicle, not driving: not firing out of it
			Leave,    // out of the stopped vehicle
		};
		struct Tasked
		{
			uint64_t at;
			Task task;
		};
		std::map<::Ped, Tasked> g_tasked; // officers given an arrest / hold task: when, which
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

		void SetWanted(int stars)
		{
			PLAYER::SET_PLAYER_WANTED_LEVEL(PLAYER::PLAYER_ID(), stars, false);
			PLAYER::SET_PLAYER_WANTED_LEVEL_NOW(PLAYER::PLAYER_ID(), false);
		}

		std::vector<::Ped> g_held; // officers the mod directs: no fire, no drive-bys, deaf to the game's events (not drivers)

		void Hold(::Ped p, bool deaf)
		{
			PED::SET_PED_SHOOT_RATE(p, 0);
			PED::SET_PED_COMBAT_ATTRIBUTES(p, 2, false); // no drive-bys
			PED::SET_BLOCKING_OF_NON_TEMPORARY_EVENTS(p, deaf);
			if (std::find(g_held.begin(), g_held.end(), p) == g_held.end())
				g_held.push_back(p);
		}

		void Give(::Ped p, ::Ped pl, Task task)
		{
			switch (task)
			{
			case Task::Arrest:
				TASK::TASK_ARREST_PED(p, pl);
				break;
			case Task::Aim:
				TASK::TASK_AIM_GUN_AT_ENTITY(p, pl, -1, false);
				break;
			case Task::Approach:
				TASK::TASK_GO_TO_ENTITY_WHILE_AIMING_AT_ENTITY(p, pl, pl, 2.0f, false, 5.0f, 8.0f, true, false, MISC::GET_HASH_KEY("FIRING_PATTERN_FULL_AUTO"));
				break;
			case Task::Seated:
				TASK::CLEAR_PED_TASKS(p);
				break;
			case Task::Leave:
				TASK::TASK_LEAVE_VEHICLE(p, PED::GET_VEHICLE_PED_IS_IN(p, false), 0);
				break;
			}
			g_tasked[p] = {ml::TickMs(), task};
		}

		bool Doing(::Ped p, Task task)
		{
			const char* name = task == Task::Arrest ? "SCRIPT_TASK_ARREST_PED"
			                 : task == Task::Aim    ? "SCRIPT_TASK_AIM_GUN_AT_ENTITY"
			                 : task == Task::Approach ? "SCRIPT_TASK_GO_TO_ENTITY_WHILE_AIMING_AT_ENTITY"
			                                          : nullptr;
			return !name || TASK::GET_SCRIPT_TASK_STATUS(p, MISC::GET_HASH_KEY(name)) != 7;
		}

		// An officer in a vehicle. The driver drives as the game has it (less aggressive for a minor offence); the others
		// do not fire out of it (no drive-bys alone does not stop them) and get out once it stops near the player (not
		// out of a helicopter).
		void InVehicle(::Ped p, ::Ped pl, int stars, float d)
		{
			const Vehicle v = PED::GET_VEHICLE_PED_IS_IN(p, false);
			if (VEHICLE::GET_PED_IN_VEHICLE_SEAT(v, -1, false) == p)
			{
				Hold(p, false); // (the game's police drive up and get out by events)
				PED::SET_DRIVER_AGGRESSIVENESS(p, stars <= 1 ? 0.25f : stars == 2 ? 0.5f : 0.8f); // no ramming for a minor offence
				return;
			}
			Hold(p, true);
			const bool out = !PED::IS_PED_IN_ANY_HELI(p) && !PED::IS_PED_IN_ANY_PLANE(p) && ENTITY::GET_ENTITY_SPEED(v) < 1.0f && d < 60.0f;
			const Task want = out ? Task::Leave : Task::Seated;
			if (const auto it = g_tasked.find(p); it == g_tasked.end() || it->second.task != want)
				Give(p, pl, want);
		}

		// Surrendered: hands up where they stood; one officer comes to arrest (the game's own arrest: busted), the others
		// keep their guns on them. Moving away calls it off.
		bool g_surrendered = false;
		Vector3 g_surrenderAt{};
		::Ped g_arrester = 0;
		float g_arresterClosest = 0;       // how close the arrester has come
		uint64_t g_arresterCloser = 0;     // and when they last came closer
		std::vector<::Ped> g_cannotArrest; // officers who got nowhere (across water, no way round)
		uint64_t g_arrestTold = 0;

		void EndSurrender(::Ped pl)
		{
			if (!g_surrendered)
				return;
			g_surrendered = false;
			g_arrester = 0;
			g_cannotArrest.clear();
			if (ENTITY::DOES_ENTITY_EXIST(pl))
				ENTITY::SET_ENTITY_INVINCIBLE(pl, false, false);
		}

		// The nearest officer on foot arrests (the rest hold their guns on the player); another one when they get nowhere.
		void ArrestBy(::Ped pl, const std::vector<::Ped>& officers)
		{
			const Vector3 me = ENTITY::GET_ENTITY_COORDS(pl, true);
			::Ped nearest = 0;
			float best = 120.0f;
			for (int round = 0; round < 2 && !nearest; ++round)
			{
				for (const ::Ped p : officers)
					if (const float d = Distance(ENTITY::GET_ENTITY_COORDS(p, true), me); d < best && !PED::IS_PED_IN_ANY_VEHICLE(p, false) &&
					    std::find(g_cannotArrest.begin(), g_cannotArrest.end(), p) == g_cannotArrest.end())
						best = d, nearest = p;
				if (!nearest)
					g_cannotArrest.clear(); // (everyone tried: again)
			}
			if (!nearest)
				return;
			g_arrester = nearest;
			g_arresterClosest = best;
			g_arresterCloser = ml::TickMs();
			TASK::CLEAR_PED_TASKS(nearest);
			Give(nearest, pl, Task::Arrest);
			ml::Log("wanted: an officer {:.0f} m off comes to arrest", best);
		}

		// The arrester still on the way (coming closer within the last 8 seconds, or there).
		bool Arresting(::Ped pl)
		{
			if (!g_arrester || !ENTITY::DOES_ENTITY_EXIST(g_arrester) || ENTITY::IS_ENTITY_DEAD(g_arrester, false) || !Doing(g_arrester, Task::Arrest))
				return false;
			const float d = Distance(ENTITY::GET_ENTITY_COORDS(g_arrester, true), ENTITY::GET_ENTITY_COORDS(pl, true));
			if (d < g_arresterClosest - 1.0f)
				g_arresterClosest = d, g_arresterCloser = ml::TickMs();
			if (d > 3.0f && ml::TickMs() - g_arresterCloser > 8000)
			{
				ml::Log("wanted: the arrester got nowhere ({:.0f} m off): another officer", d);
				g_cannotArrest.push_back(g_arrester);
				return false;
			}
			return true;
		}

		void Release()
		{
			for (auto& [p, t] : g_tasked)
				if (ENTITY::DOES_ENTITY_EXIST(p) && !ENTITY::IS_ENTITY_DEAD(p, false))
					TASK::CLEAR_PED_TASKS(p);
			g_tasked.clear();
			for (const ::Ped p : g_held)
				if (ENTITY::DOES_ENTITY_EXIST(p))
				{
					PED::SET_PED_SHOOT_RATE(p, 100);
					PED::SET_PED_COMBAT_ATTRIBUTES(p, 2, true); // drive-bys again
					PED::SET_BLOCKING_OF_NON_TEMPORARY_EVENTS(p, false);
				}
			g_held.clear();
		}

		void Say(const char* line)
		{
			HUD::BEGIN_TEXT_COMMAND_PRINT("STRING");
			HUD::ADD_TEXT_COMPONENT_SUBSTRING_PLAYER_NAME(line);
			HUD::END_TEXT_COMMAND_PRINT(3500, true);
		}

		int CopKills(int who)
		{
			int total = 0;
			for (const char* what : {"KILLS_COP", "KILLS_SWAT"})
			{
				int n = 0;
				if (STATS::STAT_GET_INT(MISC::GET_HASH_KEY(std::format("SP{}_{}", who, what).c_str()), &n, -1))
					total += n;
			}
			return total;
		}

		void WatchKills()
		{
			const uint64_t now = ml::TickMs();
			if (now < g_nextKills)
				return;
			g_nextKills = now + 500;
			const int who = ml::game::PlayerCharacterIndex();
			const int kills = who >= 0 ? CopKills(who) : -1;
			if (who == g_killsOf && kills > g_copKills && g_copKills >= 0)
			{
				g_aggressionAt = now;
				ml::Log("wanted: the player killed {} of the police", kills - g_copKills);
			}
			g_copKills = kills;
			g_killsOf = who;
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
				if (g_firedAt && now - g_firedAt < 1000 && Distance(ENTITY::GET_ENTITY_COORDS(p, true), me) < 80.0f)
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
		g_cannotArrest.clear();
		g_active = false;
		g_stars = -1;
		g_threat = -1;
		g_aggressionAt = 0;
		g_copKills = g_killsOf = -1;
	}

	void Tick()
	{
		const ::Ped pl = PLAYER::PLAYER_PED_ID();
		int stars = PLAYER::GET_PLAYER_WANTED_LEVEL(PLAYER::PLAYER_ID());
		if (g_enabled)
			WatchKills();
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
		if (PED::IS_PED_SHOOTING(pl))
			g_firedAt = ml::TickMs();
		if (!g_active)
		{
			g_active = true;
			ml::Log("wanted: the realistic police take over ({} stars)", stars);
		}
		// Surrendered: nothing counts as a new crime, and the wanted level stays at one star. (The game puts it back up
		// the moment the police see the player again - the crimes are still on record - and at two stars its police
		// shoot.)
		if (g_surrendered)
		{
			for (int crime = 1; crime <= 60; ++crime)
				PLAYER::SUPPRESS_CRIME_THIS_FRAME(PLAYER::PLAYER_ID(), crime);
			if (stars > 1)
			{
				static uint64_t told = 0;
				if (ml::TickMs() >= told)
				{
					told = ml::TickMs() + 5000;
					ml::Log("wanted: {} stars during the surrender: back to one", stars);
				}
				SetWanted(1);
				stars = 1;
			}
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
		int64_t buf[1 + 48] = {48};
		const int n = PED::GET_PED_NEARBY_PEDS(pl, reinterpret_cast<Any*>(buf), -1);
		for (int i = 0; i < n && i < 48; ++i)
			if (const ::Ped p = static_cast<::Ped>(buf[1 + i]); Officer(p))
				officers.push_back(p);
		// (officers no longer around)
		std::erase_if(g_tasked, [](const auto& t) { return !ENTITY::DOES_ENTITY_EXIST(t.first) || ENTITY::IS_ENTITY_DEAD(t.first, false); });

		const int threat = Threat(pl, officers);
		static uint64_t nextLog = 0;
		if (now >= nextLog)
		{
			nextLog = now + 5000;
			int tasked = 0;
			for (const ::Ped p : officers)
				tasked += g_tasked.contains(p);
			std::string surrendered;
			if (g_surrendered)
				surrendered = g_arrester && ENTITY::DOES_ENTITY_EXIST(g_arrester)
				                  ? std::format(", surrendered (the arrester {:.0f} m off)", Distance(ENTITY::GET_ENTITY_COORDS(g_arrester, true), me))
				                  : ", surrendered";
			ml::Log("wanted: {} stars, threat {}, {} officers near ({} given a task){}, health {}", stars, threat, officers.size(), tasked, surrendered,
			    ENTITY::GET_ENTITY_HEALTH(pl));
		}
		if (threat != g_threat)
		{
			if (threat == 2 && !g_surrendered)
			{
				Release(); // the game's full response
				ml::Log("wanted: the player went for the police: full force");
			}
			g_threat = threat;
		}

		// Surrender: hands up where they stand (unhurt meanwhile), the wanted level down to one star (giving up
		// de-escalates), one officer comes to arrest them.
		if (g_surrender.exchange(false) && !g_surrendered)
		{
			if (PED::IS_PED_IN_ANY_VEHICLE(pl, false))
				Say("先下車才能投降");
			else
			{
				g_surrendered = true;
				g_surrenderAt = me;
				g_arrester = 0;
				g_aggressionAt = 0; // (going for the police before is forgiven: only what they do from now on counts)
				g_threat = 0;
				WEAPON::SET_CURRENT_PED_WEAPON(pl, MISC::GET_HASH_KEY("WEAPON_UNARMED"), true);
				TASK::CLEAR_PED_TASKS(pl);
				TASK::TASK_HANDS_UP(pl, -1, 0, -1, 0);
				ENTITY::SET_ENTITY_INVINCIBLE(pl, true, false);
				if (stars > 1)
					SetWanted(1);
				for (const ::Ped p : officers)
					if (PED::IS_PED_IN_ANY_VEHICLE(p, false))
						InVehicle(p, pl, 1, Distance(ENTITY::GET_ENTITY_COORDS(p, true), me));
					else
					{
						Hold(p, true);
						TASK::CLEAR_PED_TASKS(p);
						Give(p, pl, Task::Aim);
					}
				ArrestBy(pl, officers);
				Say("你舉手投降了：別動，等警察過來");
				ml::Log("wanted: the player surrendered ({} officers near)", officers.size());
				return;
			}
		}
		if (g_surrendered)
		{
			if (PLAYER::IS_PLAYER_BEING_ARRESTED(PLAYER::PLAYER_ID(), true))
				return; // (cuffed and led away: the game's arrest)
			if (Distance(me, g_surrenderAt) > 4.0f || threat == 2)
			{
				EndSurrender(pl);
				Say(threat == 2 ? "你對警察動手了" : "你逃跑了");
				ml::Log("wanted: the surrender called off");
				if (threat == 2)
				{
					Release(); // the game's full response
					ml::Log("wanted: the player went for the police: full force");
				}
			}
			else
			{
				if (TASK::GET_SCRIPT_TASK_STATUS(pl, MISC::GET_HASH_KEY("SCRIPT_TASK_HANDS_UP")) == 7)
					TASK::TASK_HANDS_UP(pl, -1, 0, -1, 0);
				for (const ::Ped p : officers)
				{
					if (PED::IS_PED_IN_ANY_VEHICLE(p, false))
					{
						InVehicle(p, pl, 1, Distance(ENTITY::GET_ENTITY_COORDS(p, true), me));
						continue;
					}
					Hold(p, true);
					if (p == g_arrester)
						continue;
					if (const auto it = g_tasked.find(p); it == g_tasked.end() || it->second.task != Task::Aim || !Doing(p, Task::Aim))
						Give(p, pl, Task::Aim);
				}
				if (!Arresting(pl))
				{
					const ::Ped was = g_arrester;
					ArrestBy(pl, officers);
					if (was && was != g_arrester && ENTITY::DOES_ENTITY_EXIST(was) && !ENTITY::IS_ENTITY_DEAD(was, false))
						Give(was, pl, Task::Aim);
				}
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

		// Not armed: officers come up with their guns on the player (to a few metres) and tell them to give up - they
		// do not fire. Armed: held at gunpoint, not shot until they go for the police. Nobody fires out of a vehicle.
		const ::Ped* close = nullptr;
		for (const ::Ped& p : officers)
		{
			const float d = Distance(ENTITY::GET_ENTITY_COORDS(p, true), me);
			if (PED::IS_PED_IN_ANY_VEHICLE(p, false))
			{
				InVehicle(p, pl, stars, d);
				continue;
			}
			Hold(p, true);
			const auto it = g_tasked.find(p);
			const bool approaching = it != g_tasked.end() && it->second.task == Task::Approach;
			const Task want = threat == 1 ? Task::Aim : d > (approaching ? 8.0f : 12.0f) ? Task::Approach : Task::Aim;
			if (it == g_tasked.end() || it->second.task != want || (now - it->second.at > 8000 && !Doing(p, want)))
				Give(p, pl, want);
			if (d < 25.0f && (!close || d < Distance(ENTITY::GET_ENTITY_COORDS(*close, true), me)))
				close = &p;
		}
		if (close && now >= g_nextWord)
		{
			g_nextWord = now + 9000;
			Say(threat == 0 ? "警察：警察！不許動！雙手舉起來！（H 投降）" : "警察：放下武器！馬上放下！");
		}
	}
}
