// Services the officer calls to a scene: an ambulance for the injured and a tow truck for vehicles (the user's police
// towing: a vehicle hauled away, impounded, abandoned or wrecked).
// - Ambulance: drives in with lights and siren from a road out of sight; the paramedics tend each injured person a
//   moment and take them away; then they leave.
// - Tow truck: drives up to the vehicle, hooks it and hauls it off (gone once out of sight).
// Both: the police menu (F9: the tow truck for the vehicle nearest the officer) and the callouts (callouts.cpp).
#include "police.hpp"

#include <algorithm>
#include <cmath>
#include <vector>

namespace police::services
{
	namespace
	{
		using ai::Distance;

		bool Exists(Entity e)
		{
			return e && ENTITY::DOES_ENTITY_EXIST(e);
		}

		void RemovePed(::Ped& p)
		{
			if (Exists(p))
			{
				ENTITY::SET_ENTITY_AS_MISSION_ENTITY(p, true, true);
				PED::DELETE_PED(&p);
			}
			p = 0;
		}
		void RemoveVehicle(Vehicle& v)
		{
			if (Exists(v))
			{
				ENTITY::SET_ENTITY_AS_MISSION_ENTITY(v, true, true);
				VEHICLE::DELETE_VEHICLE(&v);
			}
			v = 0;
		}

		// A road 120-320 m from a place (the nearest road to points around it), out of the officer's sight when it can be.
		bool Road(const Vector3& to, Place& out)
		{
			bool fallback = false;
			for (int i = 0; i < 16; ++i)
			{
				const float a = static_cast<float>(i) * 0.3927f + ai::Random() * 0.3f, r = 180.0f + ai::Random() * 80.0f;
				Vector3 node{};
				float heading = 0;
				int lanes = 0;
				if (!PATH::GET_NTH_CLOSEST_VEHICLE_NODE_WITH_HEADING(to.x + std::cos(a) * r, to.y + std::sin(a) * r, to.z, 1, &node, &heading, &lanes, 0, 3.0f, 0.0f))
					continue;
				const float d = Distance(node, to);
				if (d < 120.0f || d > 320.0f || std::abs(node.z - to.z) > 40.0f)
					continue;
				const bool seen = CAMERA::IS_SPHERE_VISIBLE(node.x, node.y, node.z, 4.0f);
				if (!fallback || !seen)
				{
					out = {node.x, node.y, node.z, heading};
					fallback = true;
					if (!seen)
						return true;
				}
			}
			return fallback;
		}

		Vehicle MakeVehicle(const char* model, const Place& at)
		{
			const Hash hash = MISC::GET_HASH_KEY(model);
			const auto loaded = ml::LoadModel(hash);
			if (!loaded)
				return 0;
			const Vehicle v = VEHICLE::CREATE_VEHICLE(hash, at.x, at.y, at.z + 0.5f, at.heading, false, false, false);
			if (v)
			{
				ENTITY::SET_ENTITY_AS_MISSION_ENTITY(v, true, true);
				VEHICLE::SET_VEHICLE_ON_GROUND_PROPERLY(v, 5.0f);
				VEHICLE::SET_VEHICLE_ENGINE_ON(v, true, true, false);
			}
			return v;
		}

		::Ped Crew(Vehicle v, const char* model, int seat)
		{
			const Hash hash = MISC::GET_HASH_KEY(model);
			const auto loaded = ml::LoadModel(hash);
			if (!loaded)
				return 0;
			const ::Ped p = PED::CREATE_PED_INSIDE_VEHICLE(v, 26, hash, seat, false, false);
			if (p)
			{
				ENTITY::SET_ENTITY_AS_MISSION_ENTITY(p, true, true);
				PED::SET_BLOCKING_OF_NON_TEMPORARY_EVENTS(p, true);
				PED::SET_PED_KEEP_TASK(p, true);
			}
			return p;
		}

		void Drive(::Ped driver, Vehicle v, const Vector3& to, bool urgent)
		{
			TASK::TASK_VEHICLE_DRIVE_TO_COORD_LONGRANGE(driver, v, to.x, to.y, to.z, urgent ? 28.0f : 16.0f, urgent ? 786988 : 786603, 12.0f);
		}

		// ---- the ambulance ----------------------------------------------------------------------------------------
		enum class Medic
		{
			Coming,
			Tending,
			Leaving,
		};
		struct Medics
		{
			Vehicle car = 0;
			::Ped crew[2] = {0, 0};
			std::vector<::Ped> injured;
			Vector3 at{};
			Medic stage = Medic::Coming;
			uint64_t since = 0, nextTask = 0;
			size_t current = 0; // the injured person being tended
			bool kneeling = false;
		};
		std::vector<Medics> g_ambulances;

		void TickAmbulance(Medics& a, bool& done)
		{
			const uint64_t now = ml::TickMs();
			if (!Exists(a.car) || !Exists(a.crew[0]))
			{
				done = true;
				return;
			}
			const Vector3 carAt = ENTITY::GET_ENTITY_COORDS(a.car, true);
			switch (a.stage)
			{
			case Medic::Coming:
				if (Distance(carAt, a.at) < 18.0f || now - a.since > 150000)
				{
					VEHICLE::BRING_VEHICLE_TO_HALT(a.car, 5.0f, 2, false);
					lights::Set(a.car, 2);
					for (::Ped p : a.crew)
						if (Exists(p))
							TASK::TASK_LEAVE_VEHICLE(p, a.car, 0);
					a.stage = Medic::Tending;
					a.since = now;
					a.nextTask = now + 2500;
					ml::Log("services: the ambulance is there");
				}
				else if (now >= a.nextTask)
				{
					Drive(a.crew[0], a.car, a.at, true);
					a.nextTask = now + 5000;
				}
				break;
			case Medic::Tending:
			{
				if (now < a.nextTask)
					break;
				// Each injured person in turn: walk up, kneel a while, then they are taken.
				while (a.current < a.injured.size() && !Exists(a.injured[a.current]))
					++a.current;
				if (a.current >= a.injured.size())
				{
					for (int i = 0; i < 2; ++i)
						if (Exists(a.crew[i]))
							TASK::TASK_ENTER_VEHICLE(a.crew[i], a.car, 20000, i - 1, 1.0f, 1, nullptr);
					a.stage = Medic::Leaving;
					a.since = now;
					a.nextTask = now + 2000;
					break;
				}
				::Ped patient = a.injured[a.current];
				const Vector3 p = ENTITY::GET_ENTITY_COORDS(patient, true);
				const ::Ped medic = a.crew[0];
				if (PED::IS_PED_IN_ANY_VEHICLE(medic, false))
				{
					// Out of the ambulance first.
					for (::Ped c : a.crew)
						if (Exists(c) && PED::IS_PED_IN_ANY_VEHICLE(c, false))
							TASK::TASK_LEAVE_ANY_VEHICLE(c, 0, 0);
					a.nextTask = now + 1500;
					break;
				}
				if (!a.kneeling)
				{
					const float d = Distance(ENTITY::GET_ENTITY_COORDS(medic, true), p);
					if (d > 1.6f && now - a.since < 25000)
					{
						// Straight to them (a pavement, a road: no path needed).
						for (int i = 0; i < 2; ++i)
							if (Exists(a.crew[i]))
								TASK::TASK_GO_STRAIGHT_TO_COORD(a.crew[i], p.x + (i ? 1.2f : -0.8f), p.y + 0.6f, p.z, 1.6f, 20000, 0.0f, 0.4f);
						a.nextTask = now + 1500;
					}
					else
					{
						if (d > 1.6f)
						{
							// Could not get there (in the way): there now.
							ENTITY::SET_ENTITY_COORDS(medic, p.x - 0.8f, p.y + 0.6f, p.z, false, false, false, false);
							ml::Log("services: the paramedic put by the injured person");
						}
						TASK::TASK_TURN_PED_TO_FACE_ENTITY(medic, patient, 1000);
						TASK::TASK_START_SCENARIO_IN_PLACE(medic, "CODE_HUMAN_MEDIC_TEND_TO_DEAD", 0, true);
						if (Exists(a.crew[1]))
							TASK::TASK_START_SCENARIO_IN_PLACE(a.crew[1], "CODE_HUMAN_MEDIC_TIME_OF_DEATH", 0, true);
						a.kneeling = true;
						a.nextTask = now + 9000;
						ml::Log("services: tending injured person {}", a.current + 1);
					}
				}
				else
				{
					// Taken to the ambulance (out of the scene).
					ENTITY::SET_ENTITY_AS_MISSION_ENTITY(patient, true, true);
					PED::DELETE_PED(&a.injured[a.current]);
					for (::Ped c : a.crew)
						if (Exists(c))
							TASK::CLEAR_PED_TASKS(c);
					a.kneeling = false;
					++a.current;
					a.since = now; // (the next one's time)
					a.nextTask = now + 1500;
					records::Note("傷者由救護車送醫");
					ml::Log("services: an injured person taken to hospital");
				}
				break;
			}
			case Medic::Leaving:
				if (now >= a.nextTask && Exists(a.crew[0]) && PED::IS_PED_IN_VEHICLE(a.crew[0], a.car, false))
				{
					lights::Set(a.car, 3);
					TASK::TASK_VEHICLE_DRIVE_WANDER(a.crew[0], a.car, 25.0f, 786603);
					a.nextTask = now + 600000;
				}
				if (Distance(carAt, ENTITY::GET_ENTITY_COORDS(PLAYER::PLAYER_PED_ID(), true)) > 250.0f || now - a.since > 90000)
				{
					for (::Ped& c : a.crew)
						RemovePed(c);
					RemoveVehicle(a.car);
					done = true;
				}
				break;
			}
		}

		// ---- the tow truck ----------------------------------------------------------------------------------------
		enum class TowStage
		{
			Coming,
			Hooking,
			Hauling,
		};
		struct Truck
		{
			Vehicle truck = 0;
			::Ped driver = 0;
			Vehicle target = 0;
			TowStage stage = TowStage::Coming;
			uint64_t since = 0, nextTask = 0;
		};
		std::vector<Truck> g_trucks;

		void TickTruck(Truck& t, bool& done)
		{
			const uint64_t now = ml::TickMs();
			if (!Exists(t.truck) || !Exists(t.driver) || !Exists(t.target))
			{
				if (Exists(t.truck) && Exists(t.driver))
					TASK::TASK_VEHICLE_DRIVE_WANDER(t.driver, t.truck, 15.0f, 786603);
				done = true;
				return;
			}
			const Vector3 truckAt = ENTITY::GET_ENTITY_COORDS(t.truck, true);
			const Vector3 targetAt = ENTITY::GET_ENTITY_COORDS(t.target, true);
			switch (t.stage)
			{
			case TowStage::Coming:
				if (Distance(truckAt, targetAt) < 14.0f || now - t.since > 150000)
				{
					VEHICLE::BRING_VEHICLE_TO_HALT(t.truck, 4.0f, 2, false);
					t.stage = TowStage::Hooking;
					t.since = now;
					t.nextTask = now + 2500;
				}
				else if (now >= t.nextTask)
				{
					// Up to the vehicle's front, to hook it there.
					const Vector3 front = ENTITY::GET_OFFSET_FROM_ENTITY_IN_WORLD_COORDS(t.target, 0.0f, 7.0f, 0.0f);
					Drive(t.driver, t.truck, front, false);
					t.nextTask = now + 5000;
				}
				break;
			case TowStage::Hooking:
				if (now < t.nextTask)
					break;
				// The truck backs on to the vehicle and hooks it (placed in line with it, the hook at its front).
				{
					const float heading = ENTITY::GET_ENTITY_HEADING(t.target);
					const Vector3 ahead = ENTITY::GET_OFFSET_FROM_ENTITY_IN_WORLD_COORDS(t.target, 0.0f, 6.5f, 0.0f);
					ENTITY::SET_ENTITY_COORDS(t.truck, ahead.x, ahead.y, ahead.z + 0.3f, false, false, false, false);
					ENTITY::SET_ENTITY_HEADING(t.truck, heading);
					VEHICLE::SET_VEHICLE_ON_GROUND_PROPERLY(t.truck, 5.0f);
					VEHICLE::ATTACH_VEHICLE_TO_TOW_TRUCK(t.truck, t.target, false, 0.0f, 0.0f, 0.0f);
					VEHICLE::SET_VEHICLE_TOW_TRUCK_ARM_POSITION(t.truck, 1.0f);
				}
				TASK::TASK_VEHICLE_DRIVE_WANDER(t.driver, t.truck, 14.0f, 786603);
				t.stage = TowStage::Hauling;
				t.since = now;
				records::Note("車輛已拖吊");
				ml::Log("services: a vehicle hooked and hauled away");
				break;
			case TowStage::Hauling:
				if (Distance(truckAt, ENTITY::GET_ENTITY_COORDS(PLAYER::PLAYER_PED_ID(), true)) > 200.0f || now - t.since > 120000)
				{
					VEHICLE::DETACH_VEHICLE_FROM_ANY_TOW_TRUCK(t.target);
					RemoveVehicle(t.target);
					RemovePed(t.driver);
					RemoveVehicle(t.truck);
					done = true;
				}
				break;
			}
		}
	}

	bool Ambulance(const Vector3& at, const std::vector<::Ped>& injured)
	{
		Place road{};
		if (!Road(at, road))
			return false;
		Medics a;
		a.car = MakeVehicle("ambulance", road);
		if (!a.car)
			return false;
		a.crew[0] = Crew(a.car, "s_m_m_paramedic_01", -1);
		a.crew[1] = Crew(a.car, "s_m_m_paramedic_01", 0);
		if (!a.crew[0])
		{
			RemoveVehicle(a.car);
			return false;
		}
		lights::Set(a.car, 3);
		a.injured = injured;
		a.at = at;
		a.since = ml::TickMs();
		Drive(a.crew[0], a.car, at, true);
		a.nextTask = a.since + 5000;
		g_ambulances.push_back(a);
		ml::Log("services: an ambulance from ({:.0f}, {:.0f})", road.x, road.y);
		return true;
	}

	bool Tow(Vehicle v)
	{
		if (!Exists(v) || Towing(v))
			return false;
		// (not where another truck was just put)
		Place road{};
		bool found = false;
		for (int tries = 0; tries < 5 && !found; ++tries)
		{
			if (!Road(ENTITY::GET_ENTITY_COORDS(v, true), road))
				return false;
			found = std::none_of(g_trucks.begin(), g_trucks.end(), [&](const Truck& t) {
				return Exists(t.truck) && Distance(ENTITY::GET_ENTITY_COORDS(t.truck, true), {road.x, road.y, road.z}) < 15.0f;
			});
		}
		if (!found)
			return false;
		Truck t;
		t.truck = MakeVehicle("towtruck", road);
		if (!t.truck)
			return false;
		t.driver = Crew(t.truck, "s_m_m_trucker_01", -1);
		if (!t.driver)
		{
			RemoveVehicle(t.truck);
			return false;
		}
		ENTITY::SET_ENTITY_AS_MISSION_ENTITY(v, true, true);
		t.target = v;
		t.since = ml::TickMs();
		g_trucks.push_back(t);
		ml::Log("services: a tow truck from ({:.0f}, {:.0f})", road.x, road.y);
		return true;
	}

	bool Towing(Vehicle v)
	{
		return std::any_of(g_trucks.begin(), g_trucks.end(), [v](const Truck& t) { return t.target == v; });
	}

	bool Tending(const Vector3& at)
	{
		return std::any_of(g_ambulances.begin(), g_ambulances.end(), [&](const Medics& a) { return Distance(a.at, at) < 60.0f && a.stage != Medic::Leaving; });
	}

	menu::Item TowItem()
	{
		// The vehicle nearest the officer: empty, not police (an abandoned car, a wreck, one to impound).
		const ::Ped pl = PLAYER::PLAYER_PED_ID();
		const Vector3 me = ENTITY::GET_ENTITY_COORDS(pl, true);
		int64_t buf[1 + 16] = {16};
		const int n = PED::GET_PED_NEARBY_VEHICLES(pl, reinterpret_cast<Any*>(buf));
		Vehicle best = 0;
		float bestD = 25.0f;
		for (int i = 0; i < n && i < 16; ++i)
		{
			const Vehicle v = static_cast<Vehicle>(buf[1 + i]);
			if (!Exists(v) || VEHICLE::GET_VEHICLE_CLASS(v) == 18 || !VEHICLE::IS_VEHICLE_SEAT_FREE(v, -1, false) || Towing(v) || v == duty::PatrolCar())
				continue;
			if (const float d = Distance(ENTITY::GET_ENTITY_COORDS(v, true), me); d < bestD)
				best = v, bestD = d;
		}
		menu::Item i{"叫拖吊車", best ? records::VehicleOf(best).plate : "", best ? "把最近的這台車拖走（棄置、事故、扣押）" : "附近 25 公尺內沒有可以拖的車"};
		i.enabled = best != 0;
		i.action = [best] {
			if (Tow(best))
				ml::Notify("拖吊車在路上了");
			else
				ml::Notify("附近沒有路可以讓拖吊車過來");
		};
		return i;
	}

	void Forget()
	{
		g_ambulances.clear();
		g_trucks.clear();
	}

	void Tick()
	{
		for (size_t i = 0; i < g_ambulances.size();)
		{
			bool done = false;
			TickAmbulance(g_ambulances[i], done);
			if (done)
				g_ambulances.erase(g_ambulances.begin() + static_cast<std::ptrdiff_t>(i));
			else
				++i;
		}
		for (size_t i = 0; i < g_trucks.size();)
		{
			bool done = false;
			TickTruck(g_trucks[i], done);
			if (done)
				g_trucks.erase(g_trucks.begin() + static_cast<std::ptrdiff_t>(i));
			else
				++i;
		}
	}
}
