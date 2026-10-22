// Other police on the officer's side and the police menu (F9, user's list): backup units that drive in with sirens
// and then follow the officer, the patrol car brought to the officer by a colleague, and clearing the scene (the
// backup goes back to patrol). Dispatch (accepting or ignoring calls) waits for the callouts.
//
// The backup in a pursuit (user's design: the game's police behaving like police): the mod decides for them as for
// the suspects (ai::TakeControl). A suspect getting away by road is followed at a distance, lights and siren, without
// ramming; when the pursuit has two units, the one further behind drives ahead on the suspect's route and blocks the
// road there (stopped across it, lights on). On foot they run the suspect down without shooting. They fire only at a
// suspect who fights or shoots. Complying suspects are covered (weapon out, at a distance) for the officer to deal
// with. When nobody sees the suspect any more, they search where the suspect was last seen. What any of them sees,
// all of them know (the radio).
#include "police.hpp"

#include <algorithm>
#include <cmath>
#include <vector>

namespace police::units
{
	namespace
	{
		enum class State
		{
			Coming,  // driving in
			OnScene, // out of the car, following the officer
			Leaving, // back to the car, then away
			Pursuit, // after a suspect (see above)
			Transport, // taking a suspect in (custody.cpp)
			Fetch,     // bringing their car up to the officer (to take a suspect over)
		};
		enum class Role
		{
			None,
			Follow,   // after the suspect's car
			Block,    // a roadblock ahead of it
			OnFoot,   // after them on foot
			Engage,   // they fight or shoot
			Cover,    // they comply
			Search,   // nobody sees them
		};
		const char* RoleName(Role r)
		{
			switch (r)
			{
			case Role::None: return "none";
			case Role::Follow: return "follow";
			case Role::Block: return "block";
			case Role::OnFoot: return "on foot";
			case Role::Engage: return "engage";
			case Role::Cover: return "cover";
			case Role::Search: return "search";
			}
			return "?";
		}
		struct Unit
		{
			Vehicle car = 0;
			std::vector<::Ped> crew;
			Blip blip = 0;
			State state = State::Coming;
			uint64_t since = 0;    // when the state began
			uint64_t nextTask = 0; // when the driver's route is renewed
			Vector3 target{};      // where they are driving to
			Vector3 block{};       // the roadblock's place
			Role role = Role::None;
			int id = 0;
			Vector3 dest{};          // the transport's destination
			uint64_t eta = 0;        // when it would be there at the latest
			bool driving = false;    // the transport under way
			bool delivered = false;
			bool tagging = false;    // after the officer's car from a scene (not called in): lights as the officer's
			int colleague = -1;      // who leads it (colleagues.cpp), -1 = a unit without a name
			bool arrived = false;    // it got to the officer
		};
		int g_nextUnit = 1;
		std::vector<::Ped> g_borrowed; // officers lent out (a search): their unit leaves them be
		bool Borrowed(::Ped p)
		{
			return std::find(g_borrowed.begin(), g_borrowed.end(), p) != g_borrowed.end();
		}
		std::vector<Unit> g_units;
		// How many units dispatch sends depends on how serious it is (user's design): a quiet patrol one, a call two, a
		// pursuit or a suspect known to be armed or fighting three, shots at the police four.
		size_t MaxUnits(const char*& why)
		{
			size_t n = 1;
			why = "一般勤務";
			if (records::CaseOpen())
				n = 2, why = "處理中的案件";
			const std::vector<std::string> charges = records::Charges();
			const auto charged = [&](const char* c) { return std::find(charges.begin(), charges.end(), c) != charges.end(); };
			suspect::Target t;
			if (suspect::Pursued(t) && (t.mode == suspect::Target::Mode::Foot || t.mode == suspect::Target::Mode::Vehicle))
				n = 3, why = "追捕中";
			if (charged("非法持有武器") || charged("持有危險器械"))
				n = std::max<size_t>(n, 3), why = n == 3 ? "嫌犯持有武器" : why;
			if (suspect::Pursued(t) && t.mode == suspect::Target::Mode::Armed)
				n = 3, why = "嫌犯反抗";
			if (charged("以武器攻擊警察"))
				n = 4, why = "嫌犯開槍攻擊員警";
			if (career::May(career::Unlock::Supervise))
				++n; // a sergeant may call one unit more
			return n;
		}

		// The colleague bringing the patrol car.
		struct Valet
		{
			::Ped driver = 0;
			Vehicle car = 0;
			bool out = false;
			uint64_t since = 0;
		} g_valet;

		// Driving with lights and siren: around other vehicles, peds and objects, against the traffic when needed, not
		// stopping at lights, the shortest way.
		constexpr int kEmergency = 4 | 8 | 16 | 32 | 512 | 262144;
		constexpr int kNormal = 786603;

		bool Exists(Entity e)
		{
			return e && ENTITY::DOES_ENTITY_EXIST(e);
		}
		bool Alive(::Ped p)
		{
			return Exists(p) && !ENTITY::IS_ENTITY_DEAD(p, false);
		}
		float Distance(const Vector3& a, const Vector3& b)
		{
			return std::sqrt((a.x - b.x) * (a.x - b.x) + (a.y - b.y) * (a.y - b.y) + (a.z - b.z) * (a.z - b.z));
		}
		Vector3 PlayerAt()
		{
			return ENTITY::GET_ENTITY_COORDS(PLAYER::PLAYER_PED_ID(), true);
		}

		// A road node between `nearest` and `farthest` metres from the player that the player cannot see (so nothing appears
		// in view); the nearest out-of-sight node past `nearest` when none is.
		bool FindRoad(float nearest, float farthest, Place& out)
		{
			const Vector3 me = PlayerAt();
			bool fallback = false;
			for (int n = 1; n < 400; n += 3)
			{
				Vector3 at{};
				float heading = 0;
				int lanes = 0;
				if (!PATH::GET_NTH_CLOSEST_VEHICLE_NODE_WITH_HEADING(me.x, me.y, me.z, n, &at, &heading, &lanes, 0, 3.0f, 0.0f))
					continue;
				const float d = Distance(at, me);
				if (d < nearest || CAMERA::IS_SPHERE_VISIBLE(at.x, at.y, at.z, 4.0f))
					continue;
				if (d <= farthest || !fallback)
				{
					out = {at.x, at.y, at.z, heading};
					fallback = true;
					if (d <= farthest)
						return true;
				}
			}
			return fallback;
		}

		::Ped Crew(Vehicle car, int seat, int colleague = -1)
		{
			const Hash model = colleague >= 0 ? colleagues::Model(colleague) : MISC::GET_HASH_KEY(duty::CopModel());
			const auto loaded = ml::LoadModel(model);
			if (!loaded)
				return 0;
			const ::Ped p = PED::CREATE_PED_INSIDE_VEHICLE(car, 6 /* cop */, model, seat, false, false);
			if (!p)
				return 0;
			ENTITY::SET_ENTITY_AS_MISSION_ENTITY(p, true, true);
			if (colleague >= 0)
				colleagues::Dress(p, colleague);
			else
				PED::SET_PED_RANDOM_COMPONENT_VARIATION(p, 0);
			PED::SET_PED_RELATIONSHIP_GROUP_HASH(p, MISC::GET_HASH_KEY("COP"));
			ai::TakeControl(p);
			WEAPON::GIVE_WEAPON_TO_PED(p, MISC::GET_HASH_KEY("WEAPON_PISTOL"), 200, false, true);
			PED::SET_PED_ARMOUR(p, 50);
			PED::SET_PED_KEEP_TASK(p, true);
			return p;
		}

		void DriveTo(::Ped driver, Vehicle car, const Vector3& at, int style, float stop)
		{
			TASK::TASK_VEHICLE_DRIVE_TO_COORD_LONGRANGE(driver, car, at.x, at.y, at.z, 30.0f, style, stop);
		}

		void Release(Unit& u)
		{
			if (u.colleague >= 0)
				colleagues::SetBusy(u.colleague, false);
			if (u.blip && HUD::DOES_BLIP_EXIST(u.blip))
				HUD::REMOVE_BLIP(&u.blip);
			for (::Ped& p : u.crew)
				if (Exists(p))
				{
					ai::Release(p);
					PED::REMOVE_PED_FROM_GROUP(p);
					ENTITY::SET_PED_AS_NO_LONGER_NEEDED(&p);
				}
			if (Exists(u.car))
			{
				lights::Set(u.car, 0);
				ENTITY::SET_VEHICLE_AS_NO_LONGER_NEEDED(&u.car);
			}
		}

		bool InCar(const Unit& u, ::Ped p)
		{
			return Exists(u.car) && PED::IS_PED_IN_VEHICLE(p, u.car, false);
		}

		void Board(Unit& u)
		{
			int seat = -1;
			for (::Ped p : u.crew)
			{
				if (Alive(p) && !Borrowed(p) && !PED::IS_PED_IN_VEHICLE(p, u.car, true))
					TASK::TASK_ENTER_VEHICLE(p, u.car, 10000, seat, 2.0f, 1, nullptr);
				++seat;
			}
		}

		void Unboard(Unit& u)
		{
			for (::Ped p : u.crew)
				if (Alive(p) && InCar(u, p))
					TASK::TASK_LEAVE_VEHICLE(p, u.car, 256);
		}

		void TickUnit(Unit& u, bool& done)
		{
			const uint64_t now = ml::TickMs();
			const Vector3 me = PlayerAt();
			::Ped driver = u.crew.empty() ? 0 : u.crew[0];
			switch (u.state)
			{
			case State::Coming:
			{
				bool aboard = true;
				for (::Ped p : u.crew)
					if (Alive(p) && !Borrowed(p) && !InCar(u, p))
						aboard = false;
				if (!aboard && Exists(u.car) && Alive(driver) && now - u.since < 20000)
					break; // getting back in
				if (!Exists(u.car) || !Alive(driver) || !PED::IS_PED_IN_VEHICLE(driver, u.car, false))
				{
					u.state = State::OnScene; // whatever is left of them helps on foot
					u.tagging = false;
					for (::Ped p : u.crew)
						if (Alive(p))
						{
							TASK::CLEAR_PED_TASKS(p);
							PED::SET_PED_AS_GROUP_MEMBER(p, PLAYER::GET_PLAYER_GROUP(PLAYER::PLAYER_ID()));
						}
					break;
				}
				const Vector3 at = ENTITY::GET_ENTITY_COORDS(u.car, true);
				const ::Ped pl = PLAYER::PLAYER_PED_ID();
				const Vehicle officerCar = PED::GET_VEHICLE_PED_IS_IN(pl, true);
				const int stage = Exists(officerCar) ? lights::Stage(officerCar) : 0;
				if (Distance(at, me) < 25.0f && !PED::IS_PED_IN_ANY_VEHICLE(pl, true))
				{
					// There: stop (lights on, siren quiet), get out and join the officer.
					VEHICLE::BRING_VEHICLE_TO_HALT(u.car, 5.0f, 2, false);
					lights::Set(u.car, !u.tagging ? 2 : stage >= 2 ? 2 : 0);
					u.tagging = false;
					for (::Ped p : u.crew)
						if (Alive(p))
						{
							TASK::TASK_LEAVE_VEHICLE(p, u.car, 0);
							PED::SET_PED_AS_GROUP_MEMBER(p, PLAYER::GET_PLAYER_GROUP(PLAYER::PLAYER_ID()));
							PED::SET_PED_NEVER_LEAVES_GROUP(p, true);
						}
					u.state = State::OnScene;
					u.since = now;
					u.arrived = true;
					ml::Log("units: backup on scene");
				}
				else if (now >= u.nextTask && Distance(u.target, me) > 15.0f)
				{
					u.target = me;
					if (u.tagging)
						lights::Set(u.car, stage >= 2 ? stage : 0);
					DriveTo(driver, u.car, me, !u.tagging || stage >= 2 ? kEmergency : kNormal, 15.0f);
					u.nextTask = now + 3000;
				}
				break;
			}
			case State::OnScene:
			{
				// The officer getting in a car: nobody climbs in with them (the group would); once it drives off, back in
				// their own car and after it. On foot again: with the officer.
				const ::Ped pl = PLAYER::PLAYER_PED_ID();
				const int group = PLAYER::GET_PLAYER_GROUP(PLAYER::PLAYER_ID());
				const Vehicle officerCar = PED::GET_VEHICLE_PED_IS_USING(pl);
				if (!Exists(officerCar) || officerCar == u.car)
				{
					for (::Ped p : u.crew)
						if (Alive(p) && !Borrowed(p) && !PED::IS_PED_GROUP_MEMBER(p, group))
						{
							PED::SET_PED_AS_GROUP_MEMBER(p, group);
							PED::SET_PED_NEVER_LEAVES_GROUP(p, true);
						}
					break;
				}
				for (::Ped p : u.crew)
					if (Alive(p) && !Borrowed(p) && PED::IS_PED_GROUP_MEMBER(p, group))
					{
						PED::REMOVE_PED_FROM_GROUP(p);
						if (PED::IS_PED_IN_VEHICLE(p, officerCar, true))
							TASK::TASK_LEAVE_VEHICLE(p, officerCar, 0);
						else
							TASK::CLEAR_PED_TASKS(p);
					}
				if (ENTITY::GET_ENTITY_SPEED(officerCar) > 2.0f && Exists(u.car) && Alive(driver) &&
				    Distance(ENTITY::GET_ENTITY_COORDS(driver, true), ENTITY::GET_ENTITY_COORDS(u.car, true)) < 80.0f)
				{
					Board(u);
					u.state = State::Coming;
					u.tagging = true;
					u.since = now;
					u.target = {};
					u.nextTask = now + 2000;
					ml::Log("units: unit {} back in the car, after the officer", u.id);
				}
				break;
			}
			case State::Pursuit:
				break; // see Pursue
			case State::Fetch:
			{
				// Back in the car, then up to the place; there they wait in it.
				if (!Exists(u.car) || !Alive(driver))
					break;
				bool aboard = true;
				for (::Ped p : u.crew)
					if (Alive(p) && !PED::IS_PED_IN_VEHICLE(p, u.car, false))
						aboard = false;
				if (!u.driving && (aboard || now - u.since > 20000))
				{
					DriveTo(driver, u.car, u.dest, kNormal | 512 | 262144, 6.0f);
					u.driving = true;
				}
				break;
			}
			case State::Transport:
			{
				// Everyone aboard (or a while gone by), then off at normal speed; there once close, or once out of the
				// officer's sight for as long as the drive takes (nobody watches them drive across the map).
				if (!Exists(u.car) || !Alive(driver))
				{
					u.delivered = true;
					break;
				}
				if (!u.driving)
				{
					bool aboard = true;
					for (::Ped p : u.crew)
						if (Alive(p) && !PED::IS_PED_IN_VEHICLE(p, u.car, false))
							aboard = false;
					if (aboard || now - u.since > 25000)
					{
						const float d = Distance(ENTITY::GET_ENTITY_COORDS(u.car, true), u.dest);
						DriveTo(driver, u.car, u.dest, kNormal, 10.0f);
						u.driving = true;
						u.eta = now + static_cast<uint64_t>(d / 12.0f * 1000.0f);
						ml::Log("units: unit {} transporting, {:.0f} m to go", u.id, d);
					}
					break;
				}
				const Vector3 at = ENTITY::GET_ENTITY_COORDS(u.car, true);
				if (!u.delivered && (Distance(at, u.dest) < 30.0f || (now > u.eta && Distance(at, me) > 200.0f && !CAMERA::IS_SPHERE_VISIBLE(at.x, at.y, at.z, 4.0f))))
				{
					u.delivered = true;
					ml::Log("units: unit {} delivered the suspect", u.id);
				}
				break;
			}
			case State::Leaving:
			{
				bool aboard = Exists(u.car);
				for (::Ped p : u.crew)
					if (Alive(p) && !PED::IS_PED_IN_VEHICLE(p, u.car, false))
						aboard = false;
				const ::Ped first = [&] {
					for (::Ped p : u.crew)
						if (Alive(p))
							return p;
					return ::Ped(0);
				}();
				if (aboard || now - u.since > 30000 || !first)
				{
					if (aboard && first)
					{
						// Whoever sits at the wheel drives off on patrol.
						const ::Ped wheel = VEHICLE::GET_PED_IN_VEHICLE_SEAT(u.car, -1, false);
						if (Alive(wheel))
							TASK::TASK_VEHICLE_DRIVE_WANDER(wheel, u.car, 15.0f, kNormal);
					}
					else
						for (::Ped p : u.crew)
							if (Alive(p))
								TASK::TASK_WANDER_STANDARD(p, 10.0f, 10);
					Release(u);
					done = true;
				}
				break;
			}
			}
		}

		// The unit wants its officers back (a transport, its car, back on patrol): any lent out stop what they do.
		void Recall(Unit& u)
		{
			for (::Ped p : u.crew)
				if (Borrowed(p))
				{
					search::Drop(p);
					g_borrowed.erase(std::remove(g_borrowed.begin(), g_borrowed.end(), p), g_borrowed.end());
				}
		}

		void Dismiss(Unit& u)
		{
			if (u.state == State::Leaving)
				return;
			Recall(u);
			u.state = State::Leaving;
			u.since = ml::TickMs();
			int seat = -1;
			for (::Ped p : u.crew)
				if (Alive(p))
				{
					PED::REMOVE_PED_FROM_GROUP(p);
					if (Exists(u.car) && !PED::IS_PED_IN_VEHICLE(p, u.car, false))
						TASK::TASK_ENTER_VEHICLE(p, u.car, 20000, seat, 1.0f, 1, nullptr);
					++seat;
				}
			if (Exists(u.car))
				lights::Set(u.car, 0);
		}

		// The unit in a pursuit: its role from what the suspect does, re-given when it changes (and the driver's
		// task renewed every few seconds).
		void Pursue(Unit& u, const suspect::Target& t, Role want)
		{
			const uint64_t now = ml::TickMs();
			const bool changed = want != u.role;
			if (changed)
			{
				ml::Log("units: unit {} {} -> {}", u.id, RoleName(u.role), RoleName(want));
				u.role = want;
				u.nextTask = 0;
				for (::Ped p : u.crew)
					if (Alive(p) && !Borrowed(p))
						PED::REMOVE_PED_FROM_GROUP(p);
			}
			if (now < u.nextTask)
				return;
			u.nextTask = now + 2500;
			// In the car and the suspect further than `reach`: the driver drives up, the others sit tight (true). Closer: out.
			const auto driveUp = [&](::Ped p, float reach) {
				if (!InCar(u, p))
					return false;
				if (Distance(ENTITY::GET_ENTITY_COORDS(u.car, true), ENTITY::GET_ENTITY_COORDS(t.ped, true)) > reach)
				{
					if (p == VEHICLE::GET_PED_IN_VEHICLE_SEAT(u.car, -1, false))
						TASK::TASK_VEHICLE_DRIVE_TO_COORD_LONGRANGE(p, u.car, t.lastKnown.x, t.lastKnown.y, t.lastKnown.z, 30.0f, kEmergency, 15.0f);
				}
				else
					TASK::TASK_LEAVE_VEHICLE(p, u.car, 256);
				return true;
			};
			switch (want)
			{
			case Role::Follow:
			{
				lights::Set(u.car, 3);
				bool aboard = true;
				for (::Ped p : u.crew)
					if (Alive(p) && !InCar(u, p))
						aboard = false;
				if (!aboard)
				{
					Board(u);
					break;
				}
				const ::Ped wheel = VEHICLE::GET_PED_IN_VEHICLE_SEAT(u.car, -1, false);
				if (Alive(wheel) && Exists(t.car))
					// Follow (mission 7): keeps 15 m back, no ramming.
					TASK::TASK_VEHICLE_MISSION_PED_TARGET(wheel, u.car, t.ped, 7, 45.0f, kEmergency, 15.0f, 10.0f, true);
				break;
			}
			case Role::Block:
			{
				// Ahead on the suspect's route: the road node there, the car stopped across it.
				if (changed || Distance(u.block, t.heading) > 60.0f)
				{
					Vector3 node{};
					float heading = 0;
					int lanes = 0;
					if (PATH::GET_NTH_CLOSEST_VEHICLE_NODE_WITH_HEADING(t.heading.x, t.heading.y, t.heading.z, 1, &node, &heading, &lanes, 1, 3.0f, 0.0f))
						u.block = node;
				}
				const ::Ped wheel = Exists(u.car) ? VEHICLE::GET_PED_IN_VEHICLE_SEAT(u.car, -1, false) : 0;
				if (!Alive(wheel))
				{
					Board(u);
					break;
				}
				const Vector3 at = ENTITY::GET_ENTITY_COORDS(u.car, true);
				if (Distance(at, u.block) < 12.0f)
				{
					VEHICLE::BRING_VEHICLE_TO_HALT(u.car, 3.0f, 1, false);
					lights::Set(u.car, 2);
					// Out, behind the car.
					Unboard(u);
				}
				else
				{
					lights::Set(u.car, 3);
					TASK::TASK_VEHICLE_DRIVE_TO_COORD_LONGRANGE(wheel, u.car, u.block.x, u.block.y, u.block.z, 45.0f, kEmergency, 6.0f);
				}
				break;
			}
			case Role::OnFoot:
				if (Exists(u.car))
					lights::Set(u.car, 2);
				for (::Ped p : u.crew)
					if (Alive(p) && !Borrowed(p) && !driveUp(p, 40.0f))
						TASK::TASK_GO_TO_ENTITY(p, t.ped, -1, 1.2f, 3.0f, 0.0f, 0); // run them down, no shooting
				break;
			case Role::Engage:
				for (::Ped p : u.crew)
					if (Alive(p) && !Borrowed(p) && !driveUp(p, 60.0f))
						TASK::TASK_COMBAT_PED(p, t.ped, 0, 16);
				break;
			case Role::Cover:
				for (::Ped p : u.crew)
					if (Alive(p) && !Borrowed(p) && !driveUp(p, 40.0f))
					{
						// Weapon on them from a few metres.
						const Vector3 at = ENTITY::GET_ENTITY_COORDS(t.ped, true);
						if (Distance(ENTITY::GET_ENTITY_COORDS(p, true), at) > 7.0f)
							TASK::TASK_GO_TO_ENTITY_WHILE_AIMING_AT_ENTITY(p, t.ped, t.ped, 1.0f, false, 5.0f, 2.0f, false, false, 0);
						else
							TASK::TASK_AIM_GUN_AT_ENTITY(p, t.ped, -1, false);
					}
				break;
			case Role::Search:
				for (::Ped p : u.crew)
					if (Alive(p) && !Borrowed(p))
					{
						if (InCar(u, p))
						{
							if (p == VEHICLE::GET_PED_IN_VEHICLE_SEAT(u.car, -1, false))
							{
								if (Distance(ENTITY::GET_ENTITY_COORDS(u.car, true), t.lastKnown) > 30.0f)
									TASK::TASK_VEHICLE_DRIVE_TO_COORD_LONGRANGE(p, u.car, t.lastKnown.x, t.lastKnown.y, t.lastKnown.z, 20.0f, kNormal | 512, 15.0f);
								else
									Unboard(u);
							}
						}
						else
							TASK::TASK_WANDER_IN_AREA(p, t.lastKnown.x, t.lastKnown.y, t.lastKnown.z, 40.0f, 2.0f, 2.0f);
					}
				break;
			case Role::None:
				break;
			}
		}

		// The units in a pursuit: roles for each, and what they see goes on the radio.
		void TickPursuit()
		{
			suspect::Target t;
			const bool active = suspect::Pursued(t);
			std::vector<Unit*> engaged;
			for (Unit& u : g_units)
				if (u.state == State::OnScene || u.state == State::Pursuit || (u.state == State::Coming && active))
					engaged.push_back(&u);
			if (!active)
			{
				// Over: back with the officer.
				for (Unit* u : engaged)
					if (u->state == State::Pursuit)
					{
						u->state = State::OnScene;
						u->role = Role::None;
						for (::Ped p : u->crew)
							if (Alive(p))
							{
								TASK::CLEAR_PED_TASKS(p);
								PED::SET_PED_AS_GROUP_MEMBER(p, PLAYER::GET_PLAYER_GROUP(PLAYER::PLAYER_ID()));
							}
						ml::Log("units: unit {} back with the officer", u->id);
					}
				return;
			}
			// The radio: any of them seeing the suspect.
			for (Unit* u : engaged)
				for (::Ped p : u->crew)
					if (Alive(p) &&
					    ai::Distance(ENTITY::GET_ENTITY_COORDS(p, true), ENTITY::GET_ENTITY_COORDS(t.ped, true)) < (InCar(*u, p) ? 150.0f : 60.0f) &&
					    ENTITY::HAS_ENTITY_CLEAR_LOS_TO_ENTITY(p, t.ped, 17))
						suspect::Spotted(p);
			suspect::Pursued(t); // with what the radio told
			// The unit nearest the suspect follows; with two, the other blocks the road ahead. Roles stay put unless the
			// other unit is clearly (50 m) nearer.
			if (engaged.size() >= 2 && Exists(engaged[0]->car) && Exists(engaged[1]->car))
			{
				const Vector3 at = ENTITY::GET_ENTITY_COORDS(t.ped, true);
				const float d0 = Distance(ENTITY::GET_ENTITY_COORDS(engaged[0]->car, true), at);
				const float d1 = Distance(ENTITY::GET_ENTITY_COORDS(engaged[1]->car, true), at);
				const bool firstFollows = engaged[0]->role == Role::Follow || engaged[1]->role == Role::Block;
				if ((firstFollows && d0 > d1 + 50.0f) || (!firstFollows && d1 <= d0 + 50.0f))
					std::swap(engaged[0], engaged[1]);
			}
			for (size_t i = 0; i < engaged.size(); ++i)
			{
				Unit& u = *engaged[i];
				if (!Exists(u.car))
					continue;
				u.state = State::Pursuit;
				Role want = Role::Search;
				if (t.mode == suspect::Target::Mode::Armed)
					want = Role::Engage;
				else if (t.mode == suspect::Target::Mode::Stopped)
					want = Role::Cover;
				else if (!t.known)
					want = Role::Search;
				else if (t.mode == suspect::Target::Mode::Vehicle)
					want = i == 1 ? Role::Block : Role::Follow;
				else
					want = Role::OnFoot;
				Pursue(u, t, want);
			}
		}

		void TickValet()
		{
			if (!g_valet.driver)
				return;
			const uint64_t now = ml::TickMs();
			if (!Alive(g_valet.driver) || !Exists(g_valet.car))
			{
				if (Exists(g_valet.driver))
					ENTITY::SET_PED_AS_NO_LONGER_NEEDED(&g_valet.driver);
				g_valet = {};
				return;
			}
			if (!g_valet.out)
			{
				const Vector3 at = ENTITY::GET_ENTITY_COORDS(g_valet.car, true);
				if (Distance(at, PlayerAt()) < 12.0f || now - g_valet.since > 90000)
				{
					VEHICLE::BRING_VEHICLE_TO_HALT(g_valet.car, 3.0f, 2, false);
					TASK::TASK_LEAVE_VEHICLE(g_valet.driver, g_valet.car, 0);
					g_valet.out = true;
					g_valet.since = now;
					ml::Notify("巡邏車送到了");
					ml::Log("units: patrol car delivered after {} s", (now - g_valet.since) / 1000);
				}
				return;
			}
			if (!PED::IS_PED_IN_ANY_VEHICLE(g_valet.driver, true) || now - g_valet.since > 8000)
			{
				TASK::TASK_WANDER_STANDARD(g_valet.driver, 10.0f, 10);
				ENTITY::SET_PED_AS_NO_LONGER_NEEDED(&g_valet.driver);
				g_valet = {};
			}
		}
	}

	void CallBackup(int colleague)
	{
		size_t active = 0;
		for (const Unit& u : g_units)
			active += u.state != State::Leaving;
		const char* why = "";
		if (const size_t most = MaxUnits(why); active >= most)
		{
			ml::Notify("調度：目前狀況（{}）最多派 {} 組支援，已經有 {} 組", why, most, active);
			return;
		}
		// Who comes (a colleague on duty, as they get on with the officer) and from how far.
		float nearest = 120.0f, farthest = 300.0f;
		const int who = colleagues::PickBackup(colleague, nearest, farthest);
		Place road{};
		if (!FindRoad(nearest, farthest, road) && !FindRoad(120.0f, 300.0f, road))
		{
			ml::Notify("附近沒有路可以讓支援過來");
			return;
		}
		Unit u;
		u.car = duty::UnitCar(road);
		if (!u.car)
		{
			ml::LogError("units: no car for the backup");
			return;
		}
		u.colleague = who;
		for (const int seat : {-1, 0})
			if (const ::Ped p = Crew(u.car, seat, seat == -1 ? who : -1))
				u.crew.push_back(p);
		if (u.crew.empty())
		{
			ENTITY::SET_ENTITY_AS_MISSION_ENTITY(u.car, true, true);
			VEHICLE::DELETE_VEHICLE(&u.car);
			return;
		}
		VEHICLE::SET_VEHICLE_ENGINE_ON(u.car, true, true, false);
		lights::Set(u.car, 3);
		u.blip = HUD::ADD_BLIP_FOR_ENTITY(u.car);
		HUD::SET_BLIP_SPRITE(u.blip, 56);
		HUD::SET_BLIP_COLOUR(u.blip, 38);
		HUD::BEGIN_TEXT_COMMAND_SET_BLIP_NAME("STRING");
		HUD::ADD_TEXT_COMPONENT_SUBSTRING_PLAYER_NAME(who >= 0 ? ("支援：" + colleagues::Name(who)).c_str() : "支援");
		HUD::END_TEXT_COMMAND_SET_BLIP_NAME(u.blip);
		if (who >= 0)
			colleagues::SetBusy(who, true);
		u.since = ml::TickMs();
		u.target = PlayerAt();
		u.id = g_nextUnit++;
		DriveTo(u.crew[0], u.car, u.target, kEmergency, 15.0f);
		u.nextTask = u.since + 3000;
		g_units.push_back(u);
		const Vector3 me = PlayerAt();
		if (who >= 0)
			ml::Notify("（無線電）{} {}：收到，前往支援，距離約 {:.0f} 公尺", colleagues::Callsign(who), colleagues::Name(who), Distance({road.x, road.y, road.z}, me));
		else
			ml::Notify("調度：支援已出發，距離約 {:.0f} 公尺", Distance({road.x, road.y, road.z}, me));
		ml::Log("units: backup from ({:.0f}, {:.0f})", road.x, road.y);
	}

	void BringCar()
	{
		Vehicle car = duty::PatrolCar();
		if (!Exists(car) || ENTITY::IS_ENTITY_DEAD(car, false))
		{
			ml::Notify("你還沒有巡邏車：到警局停車場選一台");
			return;
		}
		if (g_valet.driver)
		{
			ml::Notify("巡邏車已經在路上了");
			return;
		}
		const Vector3 me = PlayerAt();
		if (Distance(ENTITY::GET_ENTITY_COORDS(car, true), me) < 40.0f)
		{
			ml::Notify("巡邏車就在附近");
			return;
		}
		if (VEHICLE::GET_PED_IN_VEHICLE_SEAT(car, -1, false))
		{
			ml::Notify("巡邏車上有人");
			return;
		}
		Place road{};
		if (!FindRoad(80.0f, 200.0f, road))
		{
			ml::Notify("附近沒有路可以把車開過來");
			return;
		}
		// A colleague drives it in from out of sight.
		ENTITY::SET_ENTITY_COORDS(car, road.x, road.y, road.z, false, false, false, false);
		ENTITY::SET_ENTITY_HEADING(car, road.heading);
		VEHICLE::SET_VEHICLE_ON_GROUND_PROPERLY(car, 5.0f);
		const ::Ped driver = Crew(car, -1);
		if (!driver)
			return;
		VEHICLE::SET_VEHICLE_ENGINE_ON(car, true, true, false);
		DriveTo(driver, car, me, kNormal | 512 | 262144, 8.0f);
		g_valet = {driver, car, false, ml::TickMs()};
		ml::Log("units: patrol car brought from ({:.0f}, {:.0f})", road.x, road.y);
		ml::Notify("同事正把巡邏車開過來");
	}

	void Clear()
	{
		if (g_units.empty())
		{
			ml::Notify("現場沒有要收的");
			return;
		}
		for (Unit& u : g_units)
		{
			if (u.colleague >= 0 && u.arrived)
				colleagues::Rapport(u.colleague, 2, "支援後道謝收隊");
			Dismiss(u);
		}
		ml::Notify("收隊：支援回去巡邏了");
	}

	size_t Backup()
	{
		size_t n = 0;
		for (const Unit& u : g_units)
			n += u.state != State::Leaving;
		return n;
	}

	void Forget()
	{
		g_borrowed.clear();
		g_units.clear();
		g_valet = {};
	}

	void Tick()
	{
		// The units stay with the officer only (and only while they are played).
		if (!officer::IsPlayer())
		{
			for (Unit& u : g_units)
				Release(u);
			g_units.clear();
			if (g_valet.driver && Exists(g_valet.driver))
				ENTITY::SET_PED_AS_NO_LONGER_NEEDED(&g_valet.driver);
			g_valet = {};
			return;
		}
		for (size_t i = 0; i < g_units.size();)
		{
			bool done = false;
			if (g_units[i].state != State::Pursuit)
				TickUnit(g_units[i], done);
			if (done)
				g_units.erase(g_units.begin() + static_cast<std::ptrdiff_t>(i));
			else
				++i;
		}
		TickPursuit();
		TickValet();
	}

	::Ped Borrow(const Vector3& around)
	{
		// The nearest, on foot or still in their car (they get out); not the driver of a car that would be left
		// standing in the road while the other sits in it, when an officer on foot is not much further.
		::Ped best = 0;
		float bestD = 120.0f;
		for (const Unit& u : g_units)
			if (u.state != State::Leaving && u.state != State::Transport)
				for (::Ped p : u.crew)
					if (Alive(p) && !Borrowed(p))
						if (const float d = Distance(ENTITY::GET_ENTITY_COORDS(p, true), around) + (PED::IS_PED_IN_ANY_VEHICLE(p, false) ? 15.0f : 0.0f); d < bestD)
							best = p, bestD = d;
		if (best)
		{
			g_borrowed.push_back(best);
			PED::REMOVE_PED_FROM_GROUP(best);
			TASK::CLEAR_PED_TASKS(best);
		}
		return best;
	}

	void Return(::Ped helper)
	{
		g_borrowed.erase(std::remove(g_borrowed.begin(), g_borrowed.end(), helper), g_borrowed.end());
		if (Alive(helper))
			PED::SET_PED_AS_GROUP_MEMBER(helper, PLAYER::GET_PLAYER_GROUP(PLAYER::PLAYER_ID()));
	}

	bool IsUnitCar(Vehicle car)
	{
		return car && std::any_of(g_units.begin(), g_units.end(), [&](const Unit& u) { return u.car == car; });
	}

	bool Transport(Vehicle car, const Vector3& to)
	{
		for (Unit& u : g_units)
			if (u.car == car && Exists(car) && u.state != State::Leaving)
			{
				if (u.state == State::Fetch && Alive(u.crew.empty() ? 0 : u.crew[0]))
					TASK::CLEAR_PED_TASKS(u.crew[0]); // (stopped where they came to)
				for (::Ped p : u.crew)
					if (Alive(p))
					{
						PED::REMOVE_PED_FROM_GROUP(p);
						TASK::CLEAR_PED_TASKS(p);
					}
				Recall(u);
				lights::Set(u.car, 0);
				u.state = State::Transport;
				u.role = Role::None;
				u.since = ml::TickMs();
				u.dest = to;
				u.driving = u.delivered = false;
				Board(u);
				return true;
			}
		return false;
	}

	Vehicle Summon(const Vector3& to)
	{
		Unit* best = nullptr;
		float bestD = 1e9f;
		for (Unit& u : g_units)
			if (Exists(u.car) && (u.state == State::OnScene || u.state == State::Pursuit || u.state == State::Coming))
				if (const float d = Distance(ENTITY::GET_ENTITY_COORDS(u.car, true), to); d < bestD && d < 600.0f)
					best = &u, bestD = d;
		if (!best)
			return 0;
		Unit& u = *best;
		for (::Ped p : u.crew)
			if (Alive(p))
			{
				PED::REMOVE_PED_FROM_GROUP(p);
				TASK::CLEAR_PED_TASKS(p);
			}
		Recall(u);
		u.state = State::Fetch;
		u.role = Role::None;
		u.since = ml::TickMs();
		u.dest = to;
		u.driving = false;
		Board(u);
		ml::Log("units: unit {} brings its car ({:.0f} m)", u.id, bestD);
		return u.car;
	}

	bool Delivered(Vehicle car)
	{
		for (const Unit& u : g_units)
			if (u.car == car)
				return u.state == State::Transport && u.delivered;
		return true; // the unit is gone
	}

	void TransportDone(Vehicle car)
	{
		for (Unit& u : g_units)
			if (u.car == car && u.state == State::Transport)
				Dismiss(u);
	}

	std::vector<Vehicle> Cars()
	{
		std::vector<Vehicle> cars;
		for (const Unit& u : g_units)
			if (Exists(u.car))
				cars.push_back(u.car);
		return cars;
	}

	int Near(const Vector3& at, float radius)
	{
		int n = 0;
		for (const Unit& u : g_units)
			for (::Ped p : u.crew)
				if (Alive(p) && Distance(ENTITY::GET_ENTITY_COORDS(p, true), at) < radius)
					++n;
		return n;
	}

	bool Covering(const Vector3& at, float radius)
	{
		for (const Unit& u : g_units)
			if (u.role == Role::Cover)
				for (::Ped p : u.crew)
					if (Alive(p) && !Borrowed(p) && !InCar(u, p) && Distance(ENTITY::GET_ENTITY_COORDS(p, true), at) < radius)
						return true;
		return false;
	}

	menu::List Menu()
	{
		menu::List m{"警察", {}};
		m.items.push_back(shift::MenuItem());
		m.items.push_back(dispatch::MenuItem());
		m.items.push_back(custody::MenuItem());
		menu::Item computer{"車上電腦", "F10", "附近車輛、查車牌、查人、案件報告（要在警車上）"};
		computer.action = mdt::Open;
		m.items.push_back(computer);
		menu::Item query{"向調度查詢", "", "用無線電查附近車輛的車牌"};
		query.sub = [] {
			menu::List q{"向調度查詢", {}};
			const ::Ped me = PLAYER::PLAYER_PED_ID();
			const Vector3 at = ENTITY::GET_ENTITY_COORDS(me, true);
			int64_t buf[1 + 20] = {20};
			const int n = PED::GET_PED_NEARBY_VEHICLES(me, reinterpret_cast<Any*>(buf));
			std::vector<std::pair<float, Vehicle>> list;
			for (int i = 0; i < n && i < 20; ++i)
				if (const Vehicle v = static_cast<Vehicle>(buf[1 + i]); v && v != PED::GET_VEHICLE_PED_IS_IN(me, false) && ENTITY::DOES_ENTITY_EXIST(v))
					list.emplace_back(Distance(at, ENTITY::GET_ENTITY_COORDS(v, true)), v);
			std::sort(list.begin(), list.end());
			if (const Vehicle ahead = mdt::Ahead())
			{
				menu::Item i{"前車 " + records::VehicleOf(ahead).plate, "", "車牌辨識讀到的前方車輛"};
				i.action = [ahead] { mdt::RadioPlate(ahead); };
				q.items.push_back(i);
			}
			for (size_t k = 0; k < list.size() && k < 6; ++k)
			{
				const Vehicle v = list[k].second;
				const char* label = VEHICLE::GET_DISPLAY_NAME_FROM_VEHICLE_MODEL(ENTITY::GET_ENTITY_MODEL(v));
				const char* text = label ? HUD::GET_FILENAME_FOR_AUDIO_CONVERSATION(label) : "";
				menu::Item i{records::VehicleOf(v).plate + "  " + (text ? text : ""), std::to_string(static_cast<int>(list[k].first)) + " m", "查這台車"};
				i.action = [v] { mdt::RadioPlate(v); };
				q.items.push_back(i);
			}
			return q;
		};
		m.items.push_back(query);
		m.items.push_back(colleagues::RadioItem());
		const char* why = "";
		const size_t most = MaxUnits(why);
		menu::Item backup{"呼叫支援", std::to_string(Backup()) + "/" + std::to_string(most) + " 組",
		    std::string("一組兩名警員開巡邏車過來（") + why + "最多 " + std::to_string(most) + " 組）"};
		backup.action = [] { CallBackup(); };
		m.items.push_back(backup);
		menu::Item car{"叫巡邏車開過來", "", "同事把你選的巡邏車開到你身邊"};
		car.action = BringCar;
		m.items.push_back(car);
		menu::Item clear{"收隊／結束目前事件", "", "支援回去巡邏"};
		clear.action = Clear;
		m.items.push_back(clear);
		return m;
	}
}
