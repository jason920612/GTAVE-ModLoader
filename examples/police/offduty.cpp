// The officer off duty (police life part 5, user's design 2026-10-22, research/phase0.md §56).
// - A car of their own: a plain sedan parked by home at first; any car they drive off duty can be made theirs (F9
//   off duty). It stays where it was left (kept in the save), shows on the map and can be called to a road nearby;
//   wrecked, the insurer puts a new one by home for $250.
// - Fatigue: hours awake add up (twice as fast on duty); tired, the officer is told; exhausted, their aim and view
//   sway. Sleeping in the bed at home (8 hours, or until an hour before the next shift) rests them, then the save.
//   Back after many hours of not being played, they slept.
// - Training: the weights at Muscle Beach (E) for strength and stamina; running and the shooting range train as the
//   game does. Skills not trained for three days drop a point a day.
// - Crime off duty: now and then in town the officer comes upon a robbery. They may deal with it (the action menu,
//   arrest, transport, as on duty) or report it for the units on duty to come (F9 off duty); dealt with, it counts
//   for the evaluation.
// The officer's skills are the SP3_* stats (the loader's fourth character: the game reads them while the officer is
// the player). Kept in the save: ml::save "officer_offduty".
#include "police.hpp"

#include <algorithm>
#include <cmath>
#include <format>
#include <string>
#include <utility>

namespace police::offduty
{
	namespace
	{
		using ai::Distance;

		const Place kHomeParking = {-664.0f, -853.7f, 23.4f, 0.0f}; // (the curb found near it is used)
		const Place kGym = {-1203.0f, -1568.5f, 4.6f, 30.0f};       // Muscle Beach weights
		constexpr int32_t kInsurer = 39;                              // the game's bank payee for car insurance (Mors Mutual)

		struct State
		{
			// The car.
			bool hasCar = false;
			int64_t model = 0;
			int primary = 0, secondary = 0;
			std::string plate;
			Place at{};
			// Fatigue (0 rested .. 100 exhausted) and when it was last counted (game minutes).
			double fatigue = 10.0;
			double counted = 0;
			// Training.
			int64_t trained[3] = {0, 0, 0}; // game day: strength, stamina, shooting
			int64_t decayed = 0;
			int last[3] = {-1, -1, -1};     // the values last seen
			double gymAt = -1e9;            // game minute of the last workout
			bool skillsSet = false;         // a new officer's skills given (out of the academy)
		};
		State g;
		bool g_loaded = false;
		Vehicle g_car = 0;
		Blip g_carBlip = 0;
		uint64_t g_nextStore = 0, g_nextCarCheck = 0, g_nextTired = 0;
		bool g_shaking = false;

		const char* const kSkills[3] = {"SP3_STRENGTH", "SP3_STAMINA", "SP3_SHOOTING_ABILITY"};
		const char* const kSkillNames[3] = {"力量", "體力", "射擊"};

		double Now()
		{
			return static_cast<double>(shift::Day()) * 1440.0 + CLOCK::GET_CLOCK_HOURS() * 60 + CLOCK::GET_CLOCK_MINUTES();
		}

		int Skill(int i)
		{
			int v = 0;
			STATS::STAT_GET_INT(MISC::GET_HASH_KEY(kSkills[i]), &v, -1);
			return v;
		}
		void SetSkill(int i, int v)
		{
			STATS::STAT_SET_INT(MISC::GET_HASH_KEY(kSkills[i]), std::clamp(v, 0, 100), true);
		}

		void Store()
		{
			ml::save::Set("officer_offduty",
			    ml::Json{{"hasCar", g.hasCar}, {"model", g.model}, {"primary", g.primary}, {"secondary", g.secondary}, {"plate", g.plate},
			        {"x", g.at.x}, {"y", g.at.y}, {"z", g.at.z}, {"h", g.at.heading}, {"fatigue", g.fatigue}, {"counted", g.counted},
			        {"trained", ml::Json::Array({g.trained[0], g.trained[1], g.trained[2]})}, {"decayed", g.decayed},
			        {"last", ml::Json::Array({g.last[0], g.last[1], g.last[2]})}, {"gymAt", g.gymAt}, {"skillsSet", g.skillsSet}});
			g_nextStore = ml::TickMs() + 10000;
		}

		void Load()
		{
			if (g_loaded)
				return;
			g_loaded = true;
			g = State{};
			const ml::Json j = ml::save::Get("officer_offduty");
			if (!j.IsObject())
				return;
			g.hasCar = j["hasCar"].Bool();
			g.model = j["model"].Int64();
			g.primary = j["primary"].Int();
			g.secondary = j["secondary"].Int();
			g.plate = j["plate"].Str();
			g.at = {j["x"].Float(), j["y"].Float(), j["z"].Float(), j["h"].Float()};
			g.fatigue = j["fatigue"].Number(10.0);
			g.counted = j["counted"].Number();
			for (int i = 0; i < 3; ++i)
			{
				g.trained[i] = j["trained"][i].Int64();
				g.last[i] = j["last"][i].Int(-1);
			}
			g.decayed = j["decayed"].Int64();
			g.gymAt = j["gymAt"].Number(-1e9);
			g.skillsSet = j["skillsSet"].Bool();
		}

		// A new officer comes out of the academy fit and trained (the SP3 skills start at 0).
		void Academy()
		{
			if (g.skillsSet)
				return;
			g.skillsSet = true;
			const std::pair<const char*, int> skills[] = {{"SP3_STRENGTH", 40}, {"SP3_STAMINA", 50}, {"SP3_SHOOTING_ABILITY", 55}, {"SP3_STEALTH_ABILITY", 25},
			    {"SP3_WHEELIE_ABILITY", 45}, {"SP3_LUNG_CAPACITY", 30}, {"SP3_FLYING_ABILITY", 5}};
			for (const auto& [stat, value] : skills)
			{
				int now = 0;
				const Hash hash = MISC::GET_HASH_KEY(stat);
				STATS::STAT_GET_INT(hash, &now, -1);
				if (now < value)
					STATS::STAT_SET_INT(hash, value, true);
			}
			Store();
			ml::Log("offduty: the officer's skills from the academy");
		}

		bool Exists(Entity e)
		{
			return e && ENTITY::DOES_ENTITY_EXIST(e);
		}

		void Help(const std::string& text)
		{
			HUD::BEGIN_TEXT_COMMAND_DISPLAY_HELP("STRING");
			HUD::ADD_TEXT_COMPONENT_SUBSTRING_PLAYER_NAME(text.c_str());
			HUD::END_TEXT_COMMAND_DISPLAY_HELP(0, false, false, -1);
		}

		// A place at the kerb of the road nearest to a point.
		bool Kerb(const Vector3& around, Place& out)
		{
			Vector3 node{};
			float heading = 0;
			int lanes = 0;
			if (!PATH::GET_NTH_CLOSEST_VEHICLE_NODE_WITH_HEADING(around.x, around.y, around.z, 1, &node, &heading, &lanes, 0, 3.0f, 0.0f))
				return false;
			const float h = heading * 0.0174533f;
			const Vector3 right{std::cos(h), std::sin(h), 0.0f};
			out = {node.x, node.y, node.z, heading};
			for (const float way : {heading, heading + 180.0f})
				if (Vector3 edge{}; PATH::GET_ROAD_BOUNDARY_USING_HEADING(node.x, node.y, node.z, way, &edge) && Distance(edge, node) < 15.0f &&
				                    (edge.x - node.x) * right.x + (edge.y - node.y) * right.y > 0.5f)
				{
					out = {edge.x - right.x * 1.2f, edge.y - right.y * 1.2f, edge.z, heading};
					break;
				}
			return true;
		}

		// ---- the car ----------------------------------------------------------------------------------------------
		void CarBlip(bool show)
		{
			if (show == (g_carBlip != 0) && (!show || HUD::DOES_BLIP_EXIST(g_carBlip)))
				return;
			if (g_carBlip && HUD::DOES_BLIP_EXIST(g_carBlip))
				HUD::REMOVE_BLIP(&g_carBlip);
			g_carBlip = 0;
			if (!show || !Exists(g_car))
				return;
			g_carBlip = HUD::ADD_BLIP_FOR_ENTITY(g_car);
			HUD::SET_BLIP_SPRITE(g_carBlip, 225); // personal vehicle
			HUD::SET_BLIP_COLOUR(g_carBlip, 3);
			HUD::SET_BLIP_AS_SHORT_RANGE(g_carBlip, false);
			HUD::BEGIN_TEXT_COMMAND_SET_BLIP_NAME("STRING");
			HUD::ADD_TEXT_COMPONENT_SUBSTRING_PLAYER_NAME("私家車");
			HUD::END_TEXT_COMMAND_SET_BLIP_NAME(g_carBlip);
		}

		void DefaultCar()
		{
			Place at{};
			if (!Kerb({kHomeParking.x, kHomeParking.y, kHomeParking.z}, at))
				at = kHomeParking;
			g.hasCar = true;
			g.model = static_cast<int64_t>(MISC::GET_HASH_KEY("premier"));
			g.primary = 0;
			g.secondary = 0;
			g.plate = "";
			g.at = at;
			Store();
			ml::Log("offduty: a car of their own by home ({:.0f}, {:.0f})", at.x, at.y);
		}

		Vehicle MakeCar(const Place& at)
		{
			const Hash model = static_cast<Hash>(g.model);
			const auto loaded = ml::LoadModel(model);
			if (!loaded)
				return 0;
			const Vehicle v = VEHICLE::CREATE_VEHICLE(model, at.x, at.y, at.z + 0.3f, at.heading, false, false, false);
			if (!v)
				return 0;
			ENTITY::SET_ENTITY_AS_MISSION_ENTITY(v, true, true);
			VEHICLE::SET_VEHICLE_ON_GROUND_PROPERLY(v, 5.0f);
			VEHICLE::SET_VEHICLE_COLOURS(v, g.primary, g.secondary);
			if (!g.plate.empty())
				VEHICLE::SET_VEHICLE_NUMBER_PLATE_TEXT(v, g.plate.c_str());
			else if (const char* p = VEHICLE::GET_VEHICLE_NUMBER_PLATE_TEXT(v))
				g.plate = p;
			VEHICLE::SET_VEHICLE_HAS_BEEN_OWNED_BY_PLAYER(v, true);
			VEHICLE::SET_VEHICLE_DOORS_LOCKED(v, 1);
			return v;
		}

		void ReleaseCar()
		{
			CarBlip(false);
			if (Exists(g_car) && !PED::IS_PED_IN_VEHICLE(PLAYER::PLAYER_PED_ID(), g_car, false))
			{
				ENTITY::SET_ENTITY_AS_MISSION_ENTITY(g_car, true, true);
				VEHICLE::DELETE_VEHICLE(&g_car);
			}
			g_car = 0;
		}

		void Car()
		{
			const ::Ped pl = PLAYER::PLAYER_PED_ID();
			const Vector3 me = ENTITY::GET_ENTITY_COORDS(pl, true);
			if (!g.hasCar)
				DefaultCar();
			if (Exists(g_car))
			{
				if (ENTITY::IS_ENTITY_DEAD(g_car, false) || VEHICLE::IS_VEHICLE_DRIVEABLE(g_car, false) == 0)
				{
					// Wrecked: the insurer puts a new one by home.
					const bool paid = ml::game::Pay(250, kInsurer, ml::game::Character::Fourth);
					ml::Notify("私家車壞了：保險公司會把新的停在家門口（自付額 ${}{}）", 250, paid ? "" : "，錢不夠，記帳");
					ENTITY::SET_VEHICLE_AS_NO_LONGER_NEEDED(&g_car);
					g_car = 0;
					CarBlip(false);
					Place at{};
					if (!Kerb({kHomeParking.x, kHomeParking.y, kHomeParking.z}, at))
						at = kHomeParking;
					g.at = at;
					Store();
					return;
				}
				// Where it is now (it stays there).
				const Vector3 at = ENTITY::GET_ENTITY_COORDS(g_car, true);
				if (Distance(at, {g.at.x, g.at.y, g.at.z}) > 5.0f)
				{
					g.at = {at.x, at.y, at.z, ENTITY::GET_ENTITY_HEADING(g_car)};
					Store();
				}
				if (Distance(at, me) > 400.0f && !PED::IS_PED_IN_VEHICLE(pl, g_car, false))
					ReleaseCar(); // made again when the officer comes back near it
				return;
			}
			if (Distance(me, {g.at.x, g.at.y, g.at.z}) < 300.0f)
			{
				g_car = MakeCar(g.at);
				if (g_car)
					ml::Log("offduty: the car where it was left");
			}
		}

		void CallCar()
		{
			const ::Ped pl = PLAYER::PLAYER_PED_ID();
			if (PED::IS_PED_IN_ANY_VEHICLE(pl, false))
			{
				ml::Notify("你已經在車上了");
				return;
			}
			const Vector3 me = ENTITY::GET_ENTITY_COORDS(pl, true);
			Place at{};
			const Vector3 ahead = ENTITY::GET_OFFSET_FROM_ENTITY_IN_WORLD_COORDS(pl, 0.0f, 25.0f, 0.0f);
			if (!Kerb(ahead, at) || Distance({at.x, at.y, at.z}, me) > 80.0f)
			{
				ml::Notify("附近沒有路可以把車送過來");
				return;
			}
			if (Exists(g_car))
			{
				ENTITY::SET_ENTITY_AS_MISSION_ENTITY(g_car, true, true);
				VEHICLE::DELETE_VEHICLE(&g_car);
			}
			CarBlip(false);
			g.at = at;
			g_car = MakeCar(at);
			Store();
			ml::Notify("私家車停在附近的路邊了");
		}

		void MakeMine()
		{
			const ::Ped pl = PLAYER::PLAYER_PED_ID();
			const Vehicle v = PED::IS_PED_IN_ANY_VEHICLE(pl, false) ? PED::GET_VEHICLE_PED_IS_IN(pl, false) : 0;
			if (!v || v == g_car)
				return;
			const int cls = VEHICLE::GET_VEHICLE_CLASS(v);
			if (cls == 18 || cls == 19 || cls == 15 || cls == 16 || cls == 14 || cls == 21)
			{
				ml::Notify("這台不能當私家車（公務車、軍用車、飛機、船）");
				return;
			}
			if (Exists(g_car))
			{
				ENTITY::SET_ENTITY_AS_MISSION_ENTITY(g_car, true, true);
				VEHICLE::DELETE_VEHICLE(&g_car);
			}
			CarBlip(false);
			g_car = v;
			ENTITY::SET_ENTITY_AS_MISSION_ENTITY(v, true, true);
			VEHICLE::SET_VEHICLE_HAS_BEEN_OWNED_BY_PLAYER(v, true);
			int p = 0, s = 0;
			VEHICLE::GET_VEHICLE_COLOURS(v, &p, &s);
			g.hasCar = true;
			g.model = static_cast<int64_t>(ENTITY::GET_ENTITY_MODEL(v));
			g.primary = p;
			g.secondary = s;
			const char* plate = VEHICLE::GET_VEHICLE_NUMBER_PLATE_TEXT(v);
			g.plate = plate ? plate : "";
			const Vector3 at = ENTITY::GET_ENTITY_COORDS(v, true);
			g.at = {at.x, at.y, at.z, ENTITY::GET_ENTITY_HEADING(v)};
			Store();
			ml::Notify("這台車現在是你的私家車了");
		}

		// ---- fatigue ----------------------------------------------------------------------------------------------
		void Fatigue()
		{
			const double now = Now();
			if (g.counted <= 0 || now < g.counted)
				g.counted = now;
			const double gap = now - g.counted;
			if (gap > 360.0)
				g.fatigue = std::min(g.fatigue, 15.0); // many hours not played: they slept
			else if (gap > 0)
				g.fatigue = std::min(100.0, g.fatigue + gap * (duty::OnDuty() ? 0.1 : 0.05));
			g.counted = now;
			const bool exhausted = g.fatigue >= 85.0;
			if (exhausted != g_shaking)
			{
				g_shaking = exhausted;
				if (exhausted)
					CAMERA::SHAKE_GAMEPLAY_CAM("DRUNK_SHAKE", 0.25f);
				else
					CAMERA::STOP_GAMEPLAY_CAM_SHAKING(false);
			}
			if (g.fatigue >= 70.0 && ml::TickMs() >= g_nextTired)
			{
				g_nextTired = ml::TickMs() + 600000;
				ml::Notify("{}", exhausted ? "累壞了：手會抖、眼睛也快睜不開，回家睡覺吧" : "有點累了，記得回家休息");
			}
		}

		// ---- training ---------------------------------------------------------------------------------------------
		void Training()
		{
			const int64_t today = shift::Day();
			for (int i = 0; i < 3; ++i)
			{
				const int v = Skill(i);
				if (g.last[i] >= 0 && v > g.last[i])
					g.trained[i] = today; // trained (the game's running, the range)
				g.last[i] = v;
				if (!g.trained[i])
					g.trained[i] = today;
			}
			// A point a day off what has not been trained for three days.
			if (g.decayed < today)
			{
				if (g.decayed)
					for (int i = 0; i < 3; ++i)
						if (today - g.trained[i] > 3)
						{
							const int days = static_cast<int>(std::min<int64_t>(today - g.decayed, 7));
							SetSkill(i, Skill(i) - days);
							g.last[i] = Skill(i);
							ml::Log("offduty: {} down {} (not trained since day {})", kSkills[i], days, g.trained[i]);
						}
				g.decayed = today;
				Store();
			}
		}

		void Gym()
		{
			const ::Ped pl = PLAYER::PLAYER_PED_ID();
			const Vector3 me = ENTITY::GET_ENTITY_COORDS(pl, true);
			if (Distance(me, {kGym.x, kGym.y, kGym.z}) > 25.0f || PED::IS_PED_IN_ANY_VEHICLE(pl, false) || menu::IsOpen())
				return;
			GRAPHICS::DRAW_MARKER(1, kGym.x, kGym.y, kGym.z - 1.0f, 0, 0, 0, 0, 0, 0, 1.0f, 1.0f, 0.6f, 93, 182, 229, 120, false, false, 2, false, nullptr,
			    nullptr, false);
			if (Distance(me, {kGym.x, kGym.y, kGym.z}) > 1.2f)
				return;
			const double now = Now();
			if (now - g.gymAt < 60.0)
			{
				Help("剛練過了，休息一下（一小時後再來）");
				return;
			}
			Help("按 ~INPUT_CONTEXT~ 健身（力量、體力）");
			if (!PAD::IS_CONTROL_JUST_PRESSED(0, 51))
				return;
			ENTITY::SET_ENTITY_HEADING(pl, kGym.heading);
			TASK::TASK_START_SCENARIO_IN_PLACE(pl, "WORLD_HUMAN_MUSCLE_FREE_WEIGHTS", 0, true);
			for (const uint64_t end = ml::TickMs() + 15000; ml::TickMs() < end;)
			{
				ml::Wait(0);
				Help("健身中……");
			}
			TASK::CLEAR_PED_TASKS(pl);
			SetSkill(0, Skill(0) + 3);
			SetSkill(1, Skill(1) + 1);
			const int64_t today = shift::Day();
			g.trained[0] = g.trained[1] = today;
			g.last[0] = Skill(0);
			g.last[1] = Skill(1);
			g.gymAt = now;
			g.fatigue = std::min(100.0, g.fatigue + 5.0);
			Store();
			ml::Notify("健身：力量 {}、體力 {}", Skill(0), Skill(1));
		}

		// ---- crime off duty ---------------------------------------------------------------------------------------
		struct Incident
		{
			int suspect = 0;
			::Ped victim = 0;
			Vector3 at{};
			uint64_t since = 0;
			bool engaged = false; // the officer came up (the case is theirs)
			bool reported = false;
		};
		Incident g_inc;
		uint64_t g_nextIncident = 0;
		bool g_forceIncident = false; // development: the next one now

		void EndIncident(const std::string& outcome, int bonus)
		{
			if (g_inc.engaged && records::CaseOpen())
			{
				const int report = records::EndCase(outcome);
				ml::Notify("報告草稿 #{} 已建立", report);
			}
			if (g_inc.reported && units::Backup())
				units::Clear(); // the units that came for it go back
			if (bonus > 0)
			{
				career::AddBonus(bonus, "下班見義勇為");
				ml::Notify("下班處理了一件搶案：考評 +{}", bonus);
			}
			if (Exists(g_inc.victim))
			{
				TASK::CLEAR_PED_TASKS(g_inc.victim);
				ENTITY::SET_PED_AS_NO_LONGER_NEEDED(&g_inc.victim);
			}
			ml::Log("offduty: robbery over: {}", outcome);
			g_inc = Incident{};
			g_nextIncident = ml::TickMs() + 720000 + static_cast<uint64_t>(ai::Random() * 480000.0f);
		}

		bool StartIncident()
		{
			const ::Ped pl = PLAYER::PLAYER_PED_ID();
			const Vector3 ahead = ENTITY::GET_OFFSET_FROM_ENTITY_IN_WORLD_COORDS(pl, (ai::Random() - 0.5f) * 60.0f, 70.0f + ai::Random() * 50.0f, 0.0f);
			Vector3 at{};
			if (!ai::OnFoot(ahead.x, ahead.y, ahead.z, at) || CAMERA::IS_SPHERE_VISIBLE(at.x, at.y, at.z, 2.0f))
			{
				g_nextIncident = ml::TickMs() + 30000;
				return false;
			}
			ai::Profile p = suspect::RandomProfile(ai::Skill::Average);
			p.armed = 1;
			p.guilt = std::max(p.guilt, 0.8f);
			p.hostility = 0.5f + ai::Random() * 0.3f;
			const int id = suspect::Spawn(at, 0.0f, p);
			if (!id)
			{
				g_nextIncident = ml::TickMs() + 60000;
				return false;
			}
			const Hash model = MISC::GET_HASH_KEY(ai::Random() < 0.5f ? "a_f_y_business_01" : "a_m_m_business_01");
			const auto loaded = ml::LoadModel(model);
			const Vector3 v = {at.x + 1.6f, at.y + 0.4f, at.z};
			const ::Ped victim = loaded ? PED::CREATE_PED(4, model, v.x, v.y, v.z, 90.0f, false, false) : 0;
			if (victim)
			{
				ENTITY::SET_ENTITY_AS_MISSION_ENTITY(victim, true, true);
				PED::SET_BLOCKING_OF_NON_TEMPORARY_EVENTS(victim, true);
				TASK::TASK_HANDS_UP(victim, -1, suspect::StatusOf(id).ped, -1, 0);
			}
			const ::Ped sp = suspect::StatusOf(id).ped;
			ENTITY::SET_ENTITY_HEADING(sp, ENTITY::GET_ENTITY_HEADING(sp));
			if (victim)
				TASK::TASK_TURN_PED_TO_FACE_ENTITY(sp, victim, -1);
			g_inc = {id, victim, at, ml::TickMs(), false, false};
			ml::Notify("下班途中：前面有人在搶劫路人。可以自己處理，或按 F9 報案請值班單位過來");
			ml::Log("offduty: a robbery at ({:.0f}, {:.0f})", at.x, at.y);
			return true;
		}

		void Incidents()
		{
			const ::Ped pl = PLAYER::PLAYER_PED_ID();
			const Vector3 me = ENTITY::GET_ENTITY_COORDS(pl, true);
			if (!g_inc.suspect)
			{
				if (!g_nextIncident)
					g_nextIncident = ml::TickMs() + 720000 + static_cast<uint64_t>(ai::Random() * 480000.0f);
				suspect::Target t;
				if (ml::TickMs() >= g_nextIncident && suspect::Count() == 0 && !life::InApartment(me) && !suspect::Pursued(t) &&
				    PLAYER::GET_PLAYER_WANTED_LEVEL(PLAYER::PLAYER_ID()) == 0 && (g_forceIncident || ai::Random() < 0.5f))
				{
					if (StartIncident())
						g_forceIncident = false;
				}
				else if (ml::TickMs() >= g_nextIncident)
					g_nextIncident = ml::TickMs() + 300000;
				return;
			}
			const suspect::Status st = suspect::StatusOf(g_inc.suspect);
			if (!g_inc.engaged && st.ped && Distance(ENTITY::GET_ENTITY_COORDS(st.ped, true), me) < 25.0f)
			{
				g_inc.engaged = true;
				records::BeginCase("下班遇到搶劫", "");
				records::Note("員警下班途中目擊搶劫");
				records::Charge("強盜");
			}
			if (std::string where; custody::Booked(g_inc.suspect, &where))
				EndIncident("嫌犯逮捕，收押於" + where, 3);
			else if (st.dead)
				EndIncident("嫌犯死亡", 1);
			else if (st.gone)
				EndIncident("嫌犯逃逸", g_inc.engaged ? 1 : 0);
			else if (!g_inc.engaged && ml::TickMs() - g_inc.since > 180000)
			{
				// Nobody came: the robber is gone with what they took.
				suspect::Release(g_inc.suspect);
				EndIncident("沒有處理，嫌犯離開", 0);
			}
			else if (Distance(g_inc.at, me) > 500.0f && !st.cuffed && !st.custody)
			{
				suspect::Release(g_inc.suspect);
				EndIncident("員警離開", 0);
			}
		}
	}

	menu::List Menu()
	{
		Load();
		menu::List m{"下班", {}};
		if (g_inc.suspect && !g_inc.reported)
		{
			menu::Item report{"報案", "搶劫", "請值班的單位過來處理（你可以先控制現場）"};
			report.action = [] {
				g_inc.reported = true;
				ml::Notify("（電話）調度：收到，派員前往。");
				units::CallBackup();
			};
			m.items.push_back(report);
		}
		menu::Item call{"叫私家車過來", "", "停到附近的路邊"};
		call.action = CallCar;
		m.items.push_back(call);
		menu::Item mine{"把這台車設為私家車", "", "現在開的這台（不能是公務車）"};
		mine.enabled = PED::IS_PED_IN_ANY_VEHICLE(PLAYER::PLAYER_PED_ID(), false);
		mine.action = MakeMine;
		m.items.push_back(mine);
		const int f = static_cast<int>(g.fatigue);
		m.items.push_back({"疲勞", std::format("{}%", f), f >= 85 ? "累壞了：回家睡覺" : f >= 70 ? "有點累了" : "精神還不錯"});
		m.items.push_back({"技能", std::format("力量 {} 體力 {} 射擊 {}", Skill(0), Skill(1), Skill(2)), "健身：海灘的重訓區；射擊：Ammu-Nation 靶場；三天沒練會退步"});
		m.items.push_back(shift::MenuItem());
		return m;
	}

	void CallNow()
	{
		g_forceIncident = true;
		g_nextIncident = 1; // due
	}

	bool Responding()
	{
		return g_inc.suspect && g_inc.engaged;
	}

	int Tiredness()
	{
		return static_cast<int>(g.fatigue);
	}

	void Sleep()
	{
		Load();
		// Eight hours, or until an hour before the next shift when that is sooner (at least four).
		int hours = 8;
		if (const int untilShift = shift::MinutesToShift(); untilShift > 0)
			hours = std::clamp((untilShift - 60) / 60, 4, 8);
		CAMERA::DO_SCREEN_FADE_OUT(800);
		for (const uint64_t end = ml::TickMs() + 3000; ml::TickMs() < end && !CAMERA::IS_SCREEN_FADED_OUT();)
			ml::Wait(0);
		CLOCK::ADD_TO_CLOCK_TIME(hours, 0, 0);
		g.fatigue = 0.0;
		g.counted = Now();
		Store();
		ml::Wait(500);
		CAMERA::DO_SCREEN_FADE_IN(800);
		ml::Notify("睡了 {} 小時，精神飽滿", hours);
	}

	ml::Json Json()
	{
		Load();
		return ml::Json{{"fatigue", static_cast<int>(g.fatigue)}, {"strength", Skill(0)}, {"stamina", Skill(1)}, {"shooting", Skill(2)},
		    {"car", g.hasCar ? std::string(VEHICLE::GET_DISPLAY_NAME_FROM_VEHICLE_MODEL(static_cast<Hash>(g.model))) : ""}};
	}

	void Loading()
	{
		g_loaded = false;
		g_car = 0;
		g_carBlip = 0;
		g_inc = Incident{};
		g_shaking = false;
	}

	void Tick()
	{
		if (!persist::Settled())
			return;
		if (!officer::IsPlayer())
		{
			ReleaseCar();
			if (g_shaking)
			{
				CAMERA::STOP_GAMEPLAY_CAM_SHAKING(true);
				g_shaking = false;
			}
			if (g_inc.suspect)
			{
				suspect::Release(g_inc.suspect);
				EndIncident("員警離開", 0);
			}
			return;
		}
		Load();
		Academy();
		if (ml::TickMs() >= g_nextCarCheck)
		{
			g_nextCarCheck = ml::TickMs() + 1000;
			Car();
			Fatigue();
			Training();
		}
		CarBlip(Exists(g_car) && !PED::IS_PED_IN_VEHICLE(PLAYER::PLAYER_PED_ID(), g_car, false));
		if (!duty::OnDuty())
		{
			Gym();
			Incidents();
		}
		else if (g_inc.suspect && !g_inc.engaged)
		{
			suspect::Release(g_inc.suspect);
			EndIncident("員警上班", 0);
		}
		if (ml::TickMs() >= g_nextStore)
			Store();
	}
}
