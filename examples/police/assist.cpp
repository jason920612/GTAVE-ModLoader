// Colleagues asking for help (police life part 3, the user's design: the AI units have incidents of their own and ask
// for backup; "officer needs help" calls every unit).
// Now and then during the officer's shift a colleague on patrol (colleagues.cpp) radios in: a suspect who will not
// comply, an agitated driver, a drunk making trouble (asking for backup: F9 to go or not), or an officer in danger, a
// suspect with a gun, shots fired (every unit goes, no asking). Where it happens the colleague's car stands with its
// lights on, the colleague holding the suspect at gunpoint; the suspect is the suspect AI's (suspect.cpp), aware once
// the officer comes up. On arrival the colleague joins the officer as backup (units.cpp; for an officer in danger a
// second unit is called too) and the case is the officer's: arrest, transport, report as usual.
// Rapport: helping +8 (+15 for an officer in danger), arriving after the suspect got away still counts half; saying no
// or not answering -2; going and not getting there in time -3 (-10).
#include "police.hpp"

#include <algorithm>
#include <cmath>
#include <format>
#include <string>

namespace police::assist
{
	namespace
	{
		using ai::Distance;

		struct Reason
		{
			const char* radio; // what the colleague says
			const char* title; // the case
			bool urgent;       // an officer in danger
			float hostility, nerve;
			int armed;
			bool drunk;
			bool stop; // a traffic stop: the driver's car is there too
		};
		const Reason kReasons[] = {
		    {"盤查的對象拒絕配合，請求支援", "嫌犯拒絕配合盤查", false, 0.55f, 0.6f, 0, false, false},
		    {"攔停的駕駛情緒很激動，請求支援", "駕駛情緒激動", false, 0.6f, 0.5f, 1, false, true},
		    {"有醉漢在鬧事，一個人壓不住，請求支援", "醉漢鬧事", false, 0.6f, 0.5f, 0, true, false},
		    {"員警需要協助！攔停的駕駛持槍，跟我對峙中！", "駕駛持槍與員警對峙", true, 0.9f, 0.85f, 2, false, true},
		    {"員警需要協助！有人開槍！", "員警遭嫌犯開槍", true, 0.95f, 0.9f, 2, false, false},
		};

		enum class Stage
		{
			None,
			Offered, // waiting for the officer's answer (F9)
			Going,   // the officer on the way
			Scene,   // the scene is there (the officer close)
		};
		struct Incident
		{
			Stage stage = Stage::None;
			int colleague = -1;
			const Reason* reason = nullptr;
			Vector3 at{};
			float heading = 0;
			uint64_t since = 0, deadline = 0;
			Blip blip = 0;
			Vehicle car = 0;
			Vehicle stopped = 0; // the car the colleague pulled over (a traffic stop)
			::Ped cop = 0;
			int suspect = 0;
			bool arrived = false;
			uint64_t closeSince = 0; // the officer near the scene since
		};
		Incident g;
		uint64_t g_next = 0;
		int g_force = -1; // development: the next one now (0 asking, 1 an officer in danger)

		constexpr uint64_t kOfferTime = 45000;

		std::string Text(const char* label)
		{
			const char* t = label ? HUD::GET_FILENAME_FOR_AUDIO_CONVERSATION(label) : nullptr;
			return t && *t && std::string(t) != "NULL" ? t : "";
		}
		std::string Zone(const Vector3& at)
		{
			const std::string z = Text(ZONE::GET_NAME_OF_ZONE(at.x, at.y, at.z));
			return z.empty() ? "市區" : z;
		}

		std::string Who()
		{
			return colleagues::Callsign(g.colleague) + " " + colleagues::Name(g.colleague);
		}

		void Radio()
		{
			AUDIO::PLAY_SOUND_FRONTEND(-1, "Event_Start_Text", "GTAO_FM_Events_Soundset", true);
		}

		void Later()
		{
			g_next = ml::TickMs() + 360000 + static_cast<uint64_t>(ai::Random() * 360000.0f); // 6-12 minutes
		}

		// Over (whatever happened): the scene's people go back to the world, the colleague to patrol.
		void Finish()
		{
			if (g.blip && HUD::DOES_BLIP_EXIST(g.blip))
				HUD::REMOVE_BLIP(&g.blip);
			if (g.arrived)
				units::Dismiss(g.colleague);
			else
			{
				if (g.cop && ENTITY::DOES_ENTITY_EXIST(g.cop))
				{
					ENTITY::SET_ENTITY_AS_MISSION_ENTITY(g.cop, true, true);
					PED::DELETE_PED(&g.cop);
				}
				if (g.car && ENTITY::DOES_ENTITY_EXIST(g.car))
				{
					ENTITY::SET_ENTITY_AS_MISSION_ENTITY(g.car, true, true);
					VEHICLE::DELETE_VEHICLE(&g.car);
				}
				if (g.suspect)
					suspect::Release(g.suspect);
				colleagues::SetBusy(g.colleague, false);
			}
			if (g.stopped && ENTITY::DOES_ENTITY_EXIST(g.stopped))
				ENTITY::SET_VEHICLE_AS_NO_LONGER_NEEDED(&g.stopped);
			g = Incident{};
			Later();
		}

		void End(const std::string& outcome, int rapport, const char* why)
		{
			if (records::CaseOpen())
			{
				const int report = records::EndCase(outcome);
				ml::Notify("報告草稿 #{} 已建立：到車上電腦補完並提交", report);
			}
			colleagues::Rapport(g.colleague, rapport, why);
			ml::Log("assist: over: {} (rapport {:+})", outcome, rapport);
			Finish();
		}

		// A road 500-900 m from the officer, in their beat when it can be.
		bool FindPlace(Vector3& at, float& heading)
		{
			const Vector3 me = ENTITY::GET_ENTITY_COORDS(PLAYER::PLAYER_PED_ID(), true);
			bool found = false;
			for (int i = 0; i < 10; ++i)
			{
				const float a = ai::Random() * 6.283f, r = 500.0f + ai::Random() * 400.0f;
				Vector3 node{};
				float h = 0;
				int lanes = 0;
				if (!PATH::GET_NTH_CLOSEST_VEHICLE_NODE_WITH_HEADING(me.x + std::cos(a) * r, me.y + std::sin(a) * r, me.z, 1, &node, &h, &lanes, 0, 3.0f, 0.0f))
					continue;
				// Not a motorway (node flag 0x40: no stopping there, and the elevated ones are out of reach), nor a junction
				// (0x80, as found on the map, research §56).
				int density = 0, flags = 0;
				PATH::GET_VEHICLE_NODE_PROPERTIES(node.x, node.y, node.z, &density, &flags);
				if (Distance(node, me) < 350.0f || (flags & 0xC0) != 0)
					continue;
				at = node, heading = h, found = true;
				if (shift::InBeat(node))
					break;
			}
			return found;
		}

		void Start()
		{
			const int who = colleagues::PickRequester();
			Vector3 at{};
			float heading = 0;
			if (who < 0 || !FindPlace(at, heading))
			{
				g_next = ml::TickMs() + 60000;
				return;
			}
			const bool urgent = g_force >= 0 ? g_force == 1 : ai::Random() < 0.25f;
			g_force = -1;
			const size_t first = urgent ? 3 : 0, count = urgent ? 2 : 3;
			g = Incident{};
			g.colleague = who;
			g.reason = &kReasons[first + static_cast<size_t>(ai::Random() * static_cast<float>(count)) % count];
			g.at = at;
			g.heading = heading;
			g.since = ml::TickMs();
			colleagues::SetBusy(who, true);
			Radio();
			if (urgent)
			{
				g.stage = Stage::Going;
				g.deadline = g.since + 240000;
				ml::Notify("（無線電）{}：{}位置{}！調度：所有單位前往支援！", Who(), g.reason->radio, Zone(at));
			}
			else
			{
				g.stage = Stage::Offered;
				ml::Notify("（無線電）{}：{}，位置{}。按 F9「同事請求支援」回應", Who(), g.reason->radio, Zone(at));
			}
			ml::Log("assist: {} asks for help ({}) at ({:.0f}, {:.0f})", colleagues::Name(who), g.reason->title, at.x, at.y);
		}

		void Go()
		{
			g.stage = Stage::Going;
			g.deadline = ml::TickMs() + 300000;
			ml::Notify("（無線電）你：{}，收到，前往支援。", colleagues::Callsign(g.colleague));
		}

		void Refuse(bool silent)
		{
			const int other = colleagues::PickRequester();
			if (other >= 0)
				ml::Notify("（無線電）{} {}：我過去支援。", colleagues::Callsign(other), colleagues::Name(other));
			colleagues::Rapport(g.colleague, -2, silent ? "請求支援沒有回應" : "請求支援被拒絕");
			ml::Log("assist: {}", silent ? "not answered" : "refused");
			Finish();
		}

		void ShowBlip()
		{
			if (g.blip && HUD::DOES_BLIP_EXIST(g.blip))
				return;
			g.blip = HUD::ADD_BLIP_FOR_COORD(g.at.x, g.at.y, g.at.z);
			HUD::SET_BLIP_SPRITE(g.blip, g.reason->urgent ? 526 : 1);
			HUD::SET_BLIP_COLOUR(g.blip, g.reason->urgent ? 1 : 3);
			HUD::SET_BLIP_ROUTE(g.blip, true);
			HUD::SET_BLIP_ROUTE_COLOUR(g.blip, g.reason->urgent ? 1 : 3);
			HUD::BEGIN_TEXT_COMMAND_SET_BLIP_NAME("STRING");
			HUD::ADD_TEXT_COMPONENT_SUBSTRING_PLAYER_NAME(("支援 " + colleagues::Name(g.colleague)).c_str());
			HUD::END_TEXT_COMMAND_SET_BLIP_NAME(g.blip);
		}

		// The scene: the colleague's car at the roadside with its lights on (and the car they pulled over ahead of it),
		// the colleague holding the suspect at gunpoint.
		bool MakeScene()
		{
			const float h = g.heading * 0.0174533f;
			const Vector3 forward{-std::sin(h), std::cos(h), 0.0f}, right{std::cos(h), std::sin(h), 0.0f};
			// The kerb on the right of the way the car goes (the boundary asked for either way round, the right one taken).
			Vector3 base = g.at;
			for (const float way : {g.heading, g.heading + 180.0f})
				if (Vector3 edge{}; PATH::GET_ROAD_BOUNDARY_USING_HEADING(g.at.x, g.at.y, g.at.z, way, &edge) && Distance(edge, g.at) < 15.0f &&
				                    (edge.x - g.at.x) * right.x + (edge.y - g.at.y) * right.y > 0.5f)
				{
					base = {edge.x - right.x * 1.4f, edge.y - right.y * 1.4f, edge.z};
					break;
				}
			const Place carAt{base.x, base.y, base.z, g.heading};
			g.car = duty::UnitCar(carAt);
			if (!g.car)
				return false;
			lights::Set(g.car, 2);
			VEHICLE::SET_VEHICLE_DOOR_OPEN(g.car, 0, false, false);
			const Hash model = colleagues::Model(g.colleague);
			const auto loaded = ml::LoadModel(model);
			const Vector3 copAt = ENTITY::GET_OFFSET_FROM_ENTITY_IN_WORLD_COORDS(g.car, -1.6f, 0.5f, 0.0f);
			g.cop = loaded ? PED::CREATE_PED(6, model, copAt.x, copAt.y, copAt.z, g.heading, false, false) : 0;
			if (!g.cop)
				return false;
			ENTITY::SET_ENTITY_AS_MISSION_ENTITY(g.cop, true, true);
			colleagues::Dress(g.cop, g.colleague);
			PED::SET_PED_RELATIONSHIP_GROUP_HASH(g.cop, MISC::GET_HASH_KEY("COP"));
			PED::SET_BLOCKING_OF_NON_TEMPORARY_EVENTS(g.cop, true);
			WEAPON::GIVE_WEAPON_TO_PED(g.cop, MISC::GET_HASH_KEY("WEAPON_PISTOL"), 200, true, true);
			PED::SET_PED_ARMOUR(g.cop, 50);
			// The car pulled over, ahead; the suspect by it (or on the pavement ahead).
			Vector3 front = ENTITY::GET_OFFSET_FROM_ENTITY_IN_WORLD_COORDS(g.car, 1.5f, 7.0f, 0.0f);
			if (g.reason->stop)
			{
				const char* const cars[] = {"emperor", "primo", "stanier", "washington", "granger", "minivan", "bison"};
				const Hash carModel = MISC::GET_HASH_KEY(cars[static_cast<size_t>(ai::Random() * std::size(cars)) % std::size(cars)]);
				if (const auto l = ml::LoadModel(carModel))
				{
					const Vector3 ahead{base.x + forward.x * 8.5f, base.y + forward.y * 8.5f, base.z};
					g.stopped = VEHICLE::CREATE_VEHICLE(carModel, ahead.x, ahead.y, ahead.z + 0.3f, g.heading, false, false, false);
					if (g.stopped)
					{
						ENTITY::SET_ENTITY_AS_MISSION_ENTITY(g.stopped, true, true);
						VEHICLE::SET_VEHICLE_ON_GROUND_PROPERLY(g.stopped, 5.0f);
						VEHICLE::SET_VEHICLE_DOOR_OPEN(g.stopped, 0, false, false);
						front = ENTITY::GET_OFFSET_FROM_ENTITY_IN_WORLD_COORDS(g.stopped, -1.4f, -1.0f, 0.0f);
					}
				}
			}
			Vector3 at = front;
			if (Vector3 snapped{}; ai::OnFoot(front.x, front.y, front.z, snapped) && Distance(snapped, front) < 3.0f)
				at = snapped;
			ai::Profile p = suspect::RandomProfile(g.reason->urgent ? ai::Skill::Veteran : ai::Skill::Average);
			p.hostility = g.reason->hostility;
			p.nerve = g.reason->nerve;
			p.armed = g.reason->armed;
			p.guilt = std::max(p.guilt, 0.6f);
			if (g.reason->drunk)
				p.impairment = ai::Impairment::Drunk;
			g.suspect = suspect::Spawn(at, g.heading + 180.0f, p);
			if (!g.suspect)
				return false;
			const ::Ped sp = suspect::StatusOf(g.suspect).ped;
			TASK::TASK_AIM_GUN_AT_ENTITY(g.cop, sp, -1, false);
			records::BeginCase(std::string("協助同事：") + g.reason->title + "（" + colleagues::Name(g.colleague) + "）", Zone(g.at));
			records::Note(colleagues::Name(g.colleague) + "請求支援：" + g.reason->radio);
			records::Involved(colleagues::Name(g.colleague) + "（員警）");
			const Vector3 carPos = ENTITY::GET_ENTITY_COORDS(g.car, true);
			ml::Log("assist: the scene is there, the car at ({:.1f}, {:.1f}, {:.1f}) heading {:.0f}", carPos.x, carPos.y, carPos.z, g.heading);
			return true;
		}

		void Arrive()
		{
			g.arrived = true;
			if (g.blip && HUD::DOES_BLIP_EXIST(g.blip))
				HUD::REMOVE_BLIP(&g.blip);
			g.blip = 0;
			TASK::CLEAR_PED_TASKS(g.cop);
			units::Adopt(g.colleague, g.car, g.cop);
			records::Note("抵達現場支援");
			ml::Notify("{}：{}", colleagues::Name(g.colleague), g.reason->urgent ? "你來了！小心，他有槍！" : "謝了，來得正好。");
			if (g.reason->urgent)
				units::CallBackup(); // every unit: another one comes
			ml::Log("assist: the officer is on scene");
		}
	}

	bool Busy()
	{
		return g.stage != Stage::None;
	}

	void CallNow(bool urgent)
	{
		g_force = urgent ? 1 : 0;
		g_next = 1; // due
	}

	menu::Item MenuItem()
	{
		if (g.stage == Stage::Offered)
		{
			menu::Item i{"同事請求支援", colleagues::Name(g.colleague), g.reason ? g.reason->radio : ""};
			i.sub = [] {
				menu::List m{"同事請求支援", {}};
				menu::Item go{"前往支援", "", Who() + "：" + (g.reason ? g.reason->radio : "")};
				go.action = [] {
					if (g.stage == Stage::Offered)
						Go();
				};
				m.items.push_back(go);
				menu::Item no{"無法前往", "", "其他單位會去，但同事會記得"};
				no.action = [] {
					if (g.stage == Stage::Offered)
						Refuse(false);
				};
				m.items.push_back(no);
				return m;
			};
			return i;
		}
		if (g.stage == Stage::Going || g.stage == Stage::Scene)
		{
			const int meters = static_cast<int>(Distance(ENTITY::GET_ENTITY_COORDS(PLAYER::PLAYER_PED_ID(), true), g.at));
			menu::Item i{"支援同事", g.arrived ? "在現場" : std::format("{} 公尺", meters), Who() + "：" + (g.reason ? g.reason->title : "")};
			i.enabled = false;
			return i;
		}
		return {};
	}

	void Forget()
	{
		g = Incident{};
		g_next = 0;
	}

	void Tick()
	{
		const uint64_t now = ml::TickMs();
		if (!officer::IsPlayer() || !duty::OnDuty())
		{
			if (g.stage != Stage::None)
				Finish();
			g_next = 0;
			return;
		}
		switch (g.stage)
		{
		case Stage::None:
			if (!g_next)
				Later();
			if (now >= g_next && dispatch::Idle() && suspect::Count() == 0 && units::Backup() == 0 && !custody::Busy())
				Start();
			break;
		case Stage::Offered:
			if (now - g.since > kOfferTime)
				Refuse(true);
			break;
		case Stage::Going:
		{
			ShowBlip();
			const float d = Distance(ENTITY::GET_ENTITY_COORDS(PLAYER::PLAYER_PED_ID(), true), g.at);
			if (d < 220.0f)
			{
				if (MakeScene())
					g.stage = Stage::Scene;
				else
				{
					ml::LogError("assist: the scene could not be made");
					Finish();
				}
			}
			else if (now > g.deadline)
			{
				ml::Notify("（無線電）{}：不用了，其他單位已經到了。", Who());
				colleagues::Rapport(g.colleague, g.reason->urgent ? -10 : -3, "支援沒有及時趕到");
				Finish();
			}
			break;
		}
		case Stage::Scene:
		{
			const Vector3 me = ENTITY::GET_ENTITY_COORDS(PLAYER::PLAYER_PED_ID(), true);
			const suspect::Status st = suspect::StatusOf(g.suspect);
			if (!g.arrived)
			{
				// The colleague holds the suspect until the officer is with them (or the suspect makes a move).
				const bool close = Distance(me, ENTITY::GET_ENTITY_COORDS(g.car, true)) < 35.0f;
				if (close && !g.closeSince)
					g.closeSince = now;
				suspect::Target t;
				const bool moving = suspect::Pursued(t) && t.ped == st.ped;
				if ((st.ped && Distance(me, ENTITY::GET_ENTITY_COORDS(st.ped, true)) < 12.0f) || moving || (g.closeSince && now - g.closeSince > 20000))
					Arrive();
				else if (now > g.deadline)
				{
					ml::Notify("（無線電）{}：不用了，其他單位已經到了。", Who());
					colleagues::Rapport(g.colleague, g.reason->urgent ? -10 : -3, "支援沒有及時趕到");
					if (records::CaseOpen())
						records::EndCase("其他單位處理");
					Finish();
				}
				break;
			}
			const int help = g.reason->urgent ? 15 : 8;
			if (std::string where; custody::Booked(g.suspect, &where))
				End("嫌犯逮捕，收押於" + where, help, "協助同事逮捕嫌犯");
			else if (st.dead)
				End("嫌犯死亡", help, "協助同事");
			else if (st.gone)
				End("嫌犯逃脫", help / 2, "趕到支援（嫌犯逃脫）");
			else if (Distance(me, g.at) > 500.0f && !st.custody && !st.cuffed)
				End("員警離開現場", -2, "支援到一半離開");
			break;
		}
		}
	}
}
