// Other police on the officer's side and the police menu (F9, user's list): backup units that drive in with sirens
// and then follow the officer, the patrol car brought to the officer by a colleague, and clearing the scene (the
// backup goes back to patrol). Dispatch (accepting or ignoring calls) waits for the callouts.
#include "police.hpp"

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
		};
		struct Unit
		{
			Vehicle car = 0;
			std::vector<::Ped> crew;
			Blip blip = 0;
			State state = State::Coming;
			uint64_t since = 0;    // when the state began
			uint64_t nextTask = 0; // when the driver's route is renewed
			Vector3 target{};      // where they are driving to
		};
		std::vector<Unit> g_units;
		constexpr size_t kMaxUnits = 2;

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

		::Ped Crew(Vehicle car, int seat)
		{
			const Hash model = MISC::GET_HASH_KEY(duty::CopModel());
			const auto loaded = ml::LoadModel(model);
			if (!loaded)
				return 0;
			const ::Ped p = PED::CREATE_PED_INSIDE_VEHICLE(car, 6 /* cop */, model, seat, false, false);
			if (!p)
				return 0;
			ENTITY::SET_ENTITY_AS_MISSION_ENTITY(p, true, true);
			PED::SET_PED_RANDOM_COMPONENT_VARIATION(p, 0);
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
			if (u.blip && HUD::DOES_BLIP_EXIST(u.blip))
				HUD::REMOVE_BLIP(&u.blip);
			for (::Ped& p : u.crew)
				if (Exists(p))
				{
					PED::REMOVE_PED_FROM_GROUP(p);
					ENTITY::SET_PED_AS_NO_LONGER_NEEDED(&p);
				}
			if (Exists(u.car))
			{
				VEHICLE::SET_VEHICLE_SIREN(u.car, false);
				ENTITY::SET_VEHICLE_AS_NO_LONGER_NEEDED(&u.car);
			}
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
				if (!Exists(u.car) || !Alive(driver) || !PED::IS_PED_IN_VEHICLE(driver, u.car, false))
				{
					u.state = State::OnScene; // whatever is left of them helps on foot
					for (::Ped p : u.crew)
						if (Alive(p))
						{
							TASK::CLEAR_PED_TASKS(p);
							PED::SET_PED_AS_GROUP_MEMBER(p, PLAYER::GET_PLAYER_GROUP(PLAYER::PLAYER_ID()));
						}
					break;
				}
				const Vector3 at = ENTITY::GET_ENTITY_COORDS(u.car, true);
				if (Distance(at, me) < 25.0f)
				{
					// There: stop (lights on, siren quiet), get out and join the officer.
					VEHICLE::BRING_VEHICLE_TO_HALT(u.car, 5.0f, 2, false);
					VEHICLE::SET_VEHICLE_HAS_MUTED_SIRENS(u.car, true);
					for (::Ped p : u.crew)
						if (Alive(p))
						{
							TASK::TASK_LEAVE_VEHICLE(p, u.car, 0);
							PED::SET_PED_AS_GROUP_MEMBER(p, PLAYER::GET_PLAYER_GROUP(PLAYER::PLAYER_ID()));
							PED::SET_PED_NEVER_LEAVES_GROUP(p, true);
						}
					u.state = State::OnScene;
					u.since = now;
					ml::Log("units: backup on scene");
				}
				else if (now >= u.nextTask && Distance(u.target, me) > 15.0f)
				{
					u.target = me;
					DriveTo(driver, u.car, me, kEmergency, 15.0f);
					u.nextTask = now + 3000;
				}
				break;
			}
			case State::OnScene:
				break;
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

		void Dismiss(Unit& u)
		{
			if (u.state == State::Leaving)
				return;
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
				VEHICLE::SET_VEHICLE_SIREN(u.car, false);
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

	void CallBackup()
	{
		size_t active = 0;
		for (const Unit& u : g_units)
			active += u.state != State::Leaving;
		if (active >= kMaxUnits)
		{
			ml::Notify("支援已經有 {} 組在路上或在現場", active);
			return;
		}
		Place road{};
		if (!FindRoad(120.0f, 300.0f, road))
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
		for (const int seat : {-1, 0})
			if (const ::Ped p = Crew(u.car, seat))
				u.crew.push_back(p);
		if (u.crew.empty())
		{
			ENTITY::SET_ENTITY_AS_MISSION_ENTITY(u.car, true, true);
			VEHICLE::DELETE_VEHICLE(&u.car);
			return;
		}
		VEHICLE::SET_VEHICLE_ENGINE_ON(u.car, true, true, false);
		VEHICLE::SET_VEHICLE_SIREN(u.car, true);
		u.blip = HUD::ADD_BLIP_FOR_ENTITY(u.car);
		HUD::SET_BLIP_SPRITE(u.blip, 56);
		HUD::SET_BLIP_COLOUR(u.blip, 38);
		HUD::BEGIN_TEXT_COMMAND_SET_BLIP_NAME("STRING");
		HUD::ADD_TEXT_COMPONENT_SUBSTRING_PLAYER_NAME("支援");
		HUD::END_TEXT_COMMAND_SET_BLIP_NAME(u.blip);
		u.since = ml::TickMs();
		u.target = PlayerAt();
		DriveTo(u.crew[0], u.car, u.target, kEmergency, 15.0f);
		u.nextTask = u.since + 3000;
		g_units.push_back(u);
		const Vector3 me = PlayerAt();
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
			Dismiss(u);
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
			TickUnit(g_units[i], done);
			if (done)
				g_units.erase(g_units.begin() + static_cast<std::ptrdiff_t>(i));
			else
				++i;
		}
		TickValet();
	}

	menu::List Menu()
	{
		menu::List m{"警察", {}};
		menu::Item dispatch{"派遣", "", "目前沒有報案（報案功能開發中）"};
		dispatch.enabled = false;
		m.items.push_back(dispatch);
		menu::Item backup{"呼叫支援", Backup() ? std::to_string(Backup()) + " 組" : "", "一組兩名警員開巡邏車過來，到場後跟著你"};
		backup.action = CallBackup;
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
