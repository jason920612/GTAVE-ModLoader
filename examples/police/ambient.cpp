// People and traffic around police work (user's design: reactions that fit the situation):
//   - Drivers ahead of a police car with its lights on (the officer's or the backup's) slow down and keep right to let it
//     through, the way drivers are told to, also without the siren.
//   - Around a stopped suspect (complying, pulled over, standing there) a few passers-by stop at a distance and watch
//     or film on their phones; when shots are fired, everyone near runs for it, away from the shooting, instead of
//     standing there.
//   - People who saw a suspect getting away remember which way they went and how (on foot, which car). The officer
//     can ask them (E next to them; they are marked when the officer is near).
#include "police.hpp"

#include <algorithm>
#include <cmath>
#include <string>
#include <vector>

namespace police::ambient
{
	namespace
	{
		using ai::Distance;

		struct Witness
		{
			::Ped ped = 0;
			uint64_t sawAt = 0;
			Vector3 where{};   // where the suspect was
			Vector3 going{};   // their direction (velocity)
			Hash car = 0;      // the vehicle they were in
			bool disguised = false;
			bool asked = false;
		};
		std::vector<Witness> g_witnesses;

		struct Watcher
		{
			::Ped ped = 0;
			uint64_t until = 0;
		};
		std::vector<Watcher> g_watchers;
		std::vector<::Ped> g_fleeing;
		uint64_t g_nextYield = 0, g_nextScene = 0, g_nextWitness = 0;

		bool Alive(::Ped p)
		{
			return p && ENTITY::DOES_ENTITY_EXIST(p) && !ENTITY::IS_ENTITY_DEAD(p, false);
		}

		// An ordinary passer-by: human, not a player, not one of the mod's (suspects and backup are mission entities),
		// not police or a medic.
		bool Ambient(::Ped p)
		{
			if (!Alive(p) || PED::IS_PED_A_PLAYER(p) || !PED::IS_PED_HUMAN(p) || ENTITY::IS_ENTITY_A_MISSION_ENTITY(p))
				return false;
			const int type = PED::GET_PED_TYPE(p);
			return type != 6 && type != 20 && type != 21 && type != 27; // cop, medic, fireman, swat
		}

		template <size_t N>
		int NearbyPeds(::Ped around, ::Ped (&out)[N])
		{
			int64_t buf[1 + N] = {static_cast<int64_t>(N)};
			const int n = std::min<int>(PED::GET_PED_NEARBY_PEDS(around, reinterpret_cast<Any*>(buf), -1), static_cast<int>(N));
			for (int i = 0; i < n; ++i)
				out[i] = static_cast<::Ped>(buf[1 + i]);
			return n;
		}

		const char* Compass(float x, float y)
		{
			static const char* const names[] = {"北", "東北", "東", "東南", "南", "西南", "西", "西北"};
			float a = std::atan2(x, y) * 57.2958f; // 0 = north (+y), 90 = east (+x)
			if (a < 0)
				a += 360.0f;
			return names[static_cast<int>((a + 22.5f) / 45.0f) % 8];
		}

		std::string CarName(Hash model)
		{
			const char* label = VEHICLE::GET_DISPLAY_NAME_FROM_VEHICLE_MODEL(model);
			const char* text = label ? HUD::GET_FILENAME_FOR_AUDIO_CONVERSATION(label) : nullptr;
			return text && *text && std::string(text) != "NULL" ? text : "車";
		}

		// ---- traffic yielding to police cars with their lights on
		void GiveWay(Vehicle police)
		{
			const ::Ped driver = VEHICLE::GET_PED_IN_VEHICLE_SEAT(police, -1, false);
			if (!driver || ENTITY::GET_ENTITY_SPEED(police) < 4.0f)
				return;
			const Vector3 at = ENTITY::GET_ENTITY_COORDS(police, true);
			const Vector3 fwd = ENTITY::GET_ENTITY_FORWARD_VECTOR(police);
			int64_t buf[1 + 12] = {12};
			const int n = PED::GET_PED_NEARBY_VEHICLES(driver, reinterpret_cast<Any*>(buf));
			for (int i = 0; i < n && i < 12; ++i)
			{
				const Vehicle v = static_cast<Vehicle>(buf[1 + i]);
				if (!v || v == police || !ENTITY::DOES_ENTITY_EXIST(v))
					continue;
				const ::Ped d = VEHICLE::GET_PED_IN_VEHICLE_SEAT(v, -1, false);
				if (!Ambient(d))
					continue;
				const Vector3 p = ENTITY::GET_ENTITY_COORDS(v, true);
				const float dx = p.x - at.x, dy = p.y - at.y;
				const float ahead = dx * fwd.x + dy * fwd.y;
				const float side = std::abs(dx * fwd.y - dy * fwd.x);
				// In front, in the same direction, in the way (or the next lane).
				const Vector3 vf = ENTITY::GET_ENTITY_FORWARD_VECTOR(v);
				if (ahead > 4.0f && ahead < 45.0f && side < 7.0f && vf.x * fwd.x + vf.y * fwd.y > 0.5f)
				{
					VEHICLE::SET_VEHICLE_INDICATOR_LIGHTS(v, 0, true);
					TASK::TASK_VEHICLE_TEMP_ACTION(d, v, 27, 2000); // keeps right and brakes for a moment
					static uint64_t logged = 0;
					if (ml::TickMs() - logged > 3000)
					{
						logged = ml::TickMs();
						ml::Log("ambient: a car gives way to a police car");
					}
				}
			}
		}

		// ---- a scene: watchers, or everyone running from gunfire
		void Scene(const suspect::Target& t)
		{
			const uint64_t now = ml::TickMs();
			const Vector3 at = ENTITY::GET_ENTITY_COORDS(t.ped, true);
			::Ped peds[24];
			const int n = NearbyPeds(t.ped, peds);
			bool shots = t.mode == suspect::Target::Mode::Armed && PED::IS_PED_SHOOTING(t.ped);
			if (!shots)
				for (int i = 0; i < n && !shots; ++i)
					shots = ENTITY::IS_ENTITY_A_MISSION_ENTITY(peds[i]) && PED::IS_PED_SHOOTING(peds[i]); // the backup firing
			if (PED::IS_PED_SHOOTING(PLAYER::PLAYER_PED_ID()))
				shots = true;
			for (int i = 0; i < n; ++i)
			{
				const ::Ped p = peds[i];
				if (!Ambient(p) || Distance(ENTITY::GET_ENTITY_COORDS(p, true), at) > 45.0f)
					continue;
				if (shots)
				{
					// Away from the shooting, for cover, at once.
					if (std::find(g_fleeing.begin(), g_fleeing.end(), p) == g_fleeing.end())
					{
						TASK::TASK_SMART_FLEE_COORD(p, at.x, at.y, at.z, 120.0f, 20000, true, false);
						PED::SET_PED_KEEP_TASK(p, true);
						g_fleeing.push_back(p);
					}
					continue;
				}
				// A calm scene: a few stop to watch, from a distance, some filming.
				if (t.mode == suspect::Target::Mode::Stopped && g_watchers.size() < 4 && !PED::IS_PED_IN_ANY_VEHICLE(p, false) &&
				    std::none_of(g_watchers.begin(), g_watchers.end(), [&](const Watcher& w) { return w.ped == p; }) && ai::Random() < 0.25f)
				{
					const float d = Distance(ENTITY::GET_ENTITY_COORDS(p, true), at);
					if (d > 8.0f && d < 30.0f)
					{
						TASK::TASK_TURN_PED_TO_FACE_ENTITY(p, t.ped, 1500);
						TASK::TASK_START_SCENARIO_IN_PLACE(p, ai::Random() < 0.5f ? "WORLD_HUMAN_MOBILE_FILM_SHOCKING" : "WORLD_HUMAN_STAND_IMPATIENT", 0, true);
						g_watchers.push_back({p, now + 25000 + static_cast<uint64_t>(ai::Random() * 30000.0f)});
					}
				}
			}
			// Watchers go on their way after a while, or when it gets loud.
			for (auto it = g_watchers.begin(); it != g_watchers.end();)
				if (!Alive(it->ped) || now >= it->until || shots || t.mode != suspect::Target::Mode::Stopped)
				{
					if (Alive(it->ped) && !shots)
					{
						TASK::CLEAR_PED_TASKS(it->ped);
						TASK::TASK_WANDER_STANDARD(it->ped, 10.0f, 10);
					}
					it = g_watchers.erase(it);
				}
				else
					++it;
		}

		// ---- witnesses: passers-by who see the suspect getting away
		void Witnesses(const suspect::Target& t)
		{
			if (t.mode != suspect::Target::Mode::Foot && t.mode != suspect::Target::Mode::Vehicle)
				return;
			const uint64_t now = ml::TickMs();
			const Vector3 at = ENTITY::GET_ENTITY_COORDS(t.ped, true);
			const Vector3 v = ENTITY::GET_ENTITY_VELOCITY(t.car ? t.car : t.ped);
			::Ped peds[24];
			const int n = NearbyPeds(t.ped, peds);
			for (int i = 0; i < n; ++i)
			{
				const ::Ped p = peds[i];
				if (!Ambient(p) || Distance(ENTITY::GET_ENTITY_COORDS(p, true), at) > 40.0f || !ENTITY::HAS_ENTITY_CLEAR_LOS_TO_ENTITY(p, t.ped, 17))
					continue;
				auto it = std::find_if(g_witnesses.begin(), g_witnesses.end(), [&](const Witness& w) { return w.ped == p; });
				if (it == g_witnesses.end())
				{
					if (g_witnesses.size() >= 8)
						continue;
					g_witnesses.push_back({p});
					it = g_witnesses.end() - 1;
					const Vector3 wp = ENTITY::GET_ENTITY_COORDS(p, true);
					ml::Log("ambient: a witness at ({:.1f}, {:.1f}, {:.1f})", wp.x, wp.y, wp.z);
				}
				it->sawAt = now;
				it->where = at;
				it->going = v;
				it->car = t.car ? ENTITY::GET_ENTITY_MODEL(t.car) : 0;
				it->asked = false;
			}
		}

		void Ask(Witness& w)
		{
			const ::Ped player = PLAYER::PLAYER_PED_ID();
			ENTITY::SET_ENTITY_AS_MISSION_ENTITY(w.ped, true, true);
			TASK::CLEAR_PED_TASKS(w.ped);
			TASK::TASK_TURN_PED_TO_FACE_ENTITY(w.ped, player, 4000);
			TASK::TASK_LOOK_AT_ENTITY(w.ped, player, 4000, 0, 2);
			const uint64_t ago = (ml::TickMs() - w.sawAt) / 1000;
			const bool moving = std::sqrt(w.going.x * w.going.x + w.going.y * w.going.y) > 1.0f;
			std::string how = w.car ? "開一台" + CarName(w.car) : "用跑的";
			std::string where = moving ? std::string("往") + Compass(w.going.x, w.going.y) + "邊" : "在那附近";
			std::string when = ago < 20 ? "剛剛" : std::to_string(ago / 60 > 0 ? ago / 60 : 1) + " 分鐘前";
			ml::Notify("目擊者：「{}有個人{}，{}{}去了。」", when, how, where, moving ? "" : "，不知道去哪");
			ml::Log("ambient: witness asked: {} {} {}", when, how, where);
			// Where they saw them, on the map.
			const Blip b = HUD::ADD_BLIP_FOR_RADIUS(w.where.x, w.where.y, w.where.z, 40.0f);
			HUD::SET_BLIP_COLOUR(b, 5);
			HUD::SET_BLIP_ALPHA(b, 120);
			HUD::SET_BLIP_DISPLAY(b, 2);
			static std::vector<std::pair<Blip, uint64_t>> marks;
			marks.emplace_back(b, ml::TickMs() + 60000);
			for (auto it = marks.begin(); it != marks.end();)
				if (ml::TickMs() >= it->second)
				{
					HUD::REMOVE_BLIP(&it->first);
					it = marks.erase(it);
				}
				else
					++it;
			w.asked = true;
			ENTITY::SET_PED_AS_NO_LONGER_NEEDED(&w.ped);
			w.ped = 0;
		}

		void TickWitnessUi()
		{
			const ::Ped player = PLAYER::PLAYER_PED_ID();
			if (PED::IS_PED_IN_ANY_VEHICLE(player, false))
				return;
			const Vector3 me = ENTITY::GET_ENTITY_COORDS(player, true);
			for (Witness& w : g_witnesses)
			{
				if (!Alive(w.ped) || w.asked)
					continue;
				const Vector3 at = ENTITY::GET_ENTITY_COORDS(w.ped, true);
				const float d = Distance(me, at);
				if (d > 20.0f)
					continue;
				GRAPHICS::DRAW_MARKER(32, at.x, at.y, at.z + 1.25f, 0, 0, 0, 0, 0, 0, 0.3f, 0.3f, 0.3f, 240, 200, 80, 180, true, true, 2, false, nullptr,
				    nullptr, false); // a question mark over them
				if (d < 2.5f)
				{
					HUD::BEGIN_TEXT_COMMAND_DISPLAY_HELP("STRING");
					HUD::ADD_TEXT_COMPONENT_SUBSTRING_PLAYER_NAME("按 ~INPUT_CONTEXT~ 詢問目擊者");
					HUD::END_TEXT_COMMAND_DISPLAY_HELP(0, false, false, -1);
					if (PAD::IS_CONTROL_JUST_PRESSED(0, 51))
						Ask(w);
					return;
				}
			}
		}
	}

	void Forget()
	{
		g_witnesses.clear();
		g_watchers.clear();
		g_fleeing.clear();
	}

	void Tick()
	{
		if (!officer::IsPlayer())
			return;
		const uint64_t now = ml::TickMs();
		// Traffic: the officer's car and the backup's, lights on.
		if (now >= g_nextYield)
		{
			g_nextYield = now + 400;
			const ::Ped player = PLAYER::PLAYER_PED_ID();
			if (PED::IS_PED_IN_ANY_VEHICLE(player, false))
			{
				const Vehicle v = PED::GET_VEHICLE_PED_IS_IN(player, false);
				if (lights::Stage(v) >= 2)
					GiveWay(v);
			}
			for (const Vehicle v : units::Cars())
				if (lights::Stage(v) >= 2)
					GiveWay(v);
		}
		suspect::Target t;
		const bool active = suspect::Pursued(t);
		if (active && now >= g_nextScene)
		{
			g_nextScene = now + 700;
			Scene(t);
		}
		if (!active && !g_watchers.empty())
		{
			for (Watcher& w : g_watchers)
				if (Alive(w.ped))
				{
					TASK::CLEAR_PED_TASKS(w.ped);
					TASK::TASK_WANDER_STANDARD(w.ped, 10.0f, 10);
				}
			g_watchers.clear();
		}
		if (!active)
			g_fleeing.clear();
		if (active && now >= g_nextWitness)
		{
			g_nextWitness = now + 500;
			Witnesses(t);
		}
		// Witnesses are remembered for five minutes.
		g_witnesses.erase(std::remove_if(g_witnesses.begin(), g_witnesses.end(),
		                      [&](const Witness& w) { return !Alive(w.ped) || now - w.sawAt > 300000; }),
		    g_witnesses.end());
		TickWitnessUi();
	}
}
