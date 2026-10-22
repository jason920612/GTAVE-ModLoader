// Dispatch and callouts (user's design: calls come over the radio every few minutes while on duty; the officer accepts
// or ignores them in the police menu, F9). The first callout: a stolen car reported on the move, somewhere in the
// city; dispatch narrows down where it was last reported until the officer finds it. What the driver does when the
// officer lights them up is theirs to decide (suspect.cpp). The thief is taking the car somewhere a few kilometres on to
// leave it there, and waits nearby a while: the officer may find the car parked, with the thief close by or gone. A car
// not found in 15 minutes is lost track of (the call ends).
//
// The action menu (hold E next to a suspect, user's design): order them out of the vehicle, search them, check their
// ID, cuff them, put them in the patrol car (or the backup's, for them to take in), or let them go. A suspect booked at a
// station or the prison (custody.cpp) ends the call as an arrest.
#include "police.hpp"

#include <algorithm>
#include <cmath>
#include <string>
#include <utility>
#include <vector>

namespace police::dispatch
{
	namespace
	{
		using ai::Distance;

		enum class Call
		{
			None,
			Offered,
			Active,
		};
		Call g_call = Call::None;
		int g_suspect = 0;
		uint64_t g_offeredAt = 0, g_nextCall = 0, g_reportAt = 0;
		Blip g_area = 0;
		bool g_found = false;
		std::string g_carName;
		Vehicle g_car = 0;          // the stolen car (kept while the call lasts, whoever is in it)
		uint64_t g_acceptedAt = 0;
		bool g_easy = false; // development: the next call's suspect gives up
		// Where the thief takes the car: a road a few kilometres on, looked up once the path nodes there are loaded.
		struct DropOff
		{
			Vector3 from{}, at{};
			uint64_t since = 0;
			int tries = 0;
			bool done = true;
		} g_dropOff;

		constexpr uint64_t kOfferTime = 45000;      // an offer stands this long
		constexpr uint64_t kLostAfter = 15 * 60000; // a car not found by then is lost track of

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

		const char* Compass(float x, float y)
		{
			static const char* const names[] = {"北", "東北", "東", "東南", "南", "西南", "西", "西北"};
			float a = std::atan2(x, y) * 57.2958f;
			if (a < 0)
				a += 360.0f;
			return names[static_cast<int>((a + 22.5f) / 45.0f) % 8];
		}

		void Radio()
		{
			AUDIO::PLAY_SOUND_FRONTEND(-1, "Event_Start_Text", "GTAO_FM_Events_Soundset", true);
		}

		void ClearArea()
		{
			if (g_area)
				HUD::REMOVE_BLIP(&g_area);
			g_area = 0;
		}

		// The call over: the stolen car is the world's again.
		void LetCarGo()
		{
			if (g_car && ENTITY::DOES_ENTITY_EXIST(g_car) && VEHICLE::GET_VEHICLE_CLASS(g_car) != 18)
				ENTITY::SET_VEHICLE_AS_NO_LONGER_NEEDED(&g_car);
			g_car = 0;
			g_dropOff.done = true;
		}

		void End(const char* how, const char* outcome = nullptr)
		{
			ClearArea();
			if (records::CaseOpen())
			{
				const int report = records::EndCase(outcome ? outcome : how);
				ml::Notify("報告草稿 #{} 已建立：到車上電腦補完並提交", report);
			}
			ml::Notify("調度：{}", how);
			ml::Log("dispatch: call over: {}", how);
			LetCarGo();
			g_call = Call::None;
			g_suspect = 0;
			g_nextCall = ml::TickMs() + 120000 + static_cast<uint64_t>(ai::Random() * 180000.0f);
		}

		// The next place to try for where the thief takes the car: 2-4 km on from where they were.
		void NextDropOff()
		{
			const float a = ai::Random() * 6.283f, r = 2000.0f + ai::Random() * 2000.0f;
			g_dropOff.at = {g_dropOff.from.x + std::cos(a) * r, g_dropOff.from.y + std::sin(a) * r, g_dropOff.from.z};
			g_dropOff.since = ml::TickMs();
			++g_dropOff.tries;
		}

		// Every frame until done: the road nearest the place tried once its path nodes are in, or the next place (the sea,
		// the hills, not loaded in time). Not a motorway, if it can be helped.
		void PlanDropOff()
		{
			if (g_dropOff.done)
				return;
			const Vector3 c = g_dropOff.at;
			constexpr float r = 200.0f;
			PATH::REQUEST_PATH_NODES_IN_AREA_THIS_FRAME(c.x - r, c.y - r, c.x + r, c.y + r);
			if (PATH::ARE_NODES_LOADED_FOR_AREA(c.x - r, c.y - r, c.x + r, c.y + r))
			{
				Vector3 node{};
				float heading = 0;
				int lanes = 0;
				if (PATH::GET_NTH_CLOSEST_VEHICLE_NODE_WITH_HEADING(c.x, c.y, c.z, 1, &node, &heading, &lanes, 0, 3.0f, 0.0f) &&
				    std::hypot(node.x - c.x, node.y - c.y) < 300.0f && Distance(node, g_dropOff.from) > 1500.0f && (lanes <= 2 || g_dropOff.tries > 5))
				{
					g_dropOff.done = true;
					suspect::HeadFor(g_suspect, node);
					ml::Log("dispatch: the thief takes the car to ({:.0f}, {:.0f}), {:.0f} m on ({} lanes)", node.x, node.y, Distance(node, g_dropOff.from), lanes);
					return;
				}
			}
			else if (ml::TickMs() - g_dropOff.since < 3000)
				return;
			if (g_dropOff.tries >= 8)
			{
				g_dropOff.done = true;
				ml::Log("dispatch: no place found for the thief to take the car: drives around");
				return;
			}
			NextDropOff();
		}

		// A stolen car somewhere on the roads 500-900 m away.
		void Offer()
		{
			const Vector3 me = ENTITY::GET_ENTITY_COORDS(PLAYER::PLAYER_PED_ID(), true);
			const float a = ai::Random() * 6.283f, r = 500.0f + ai::Random() * 400.0f;
			ai::Profile p = suspect::RandomProfile(ai::Random() < 0.3f ? ai::Skill::Veteran : ai::Random() < 0.5f ? ai::Skill::Novice : ai::Skill::Average);
			p.guilt = std::max(p.guilt, 0.7f + ai::Random() * 0.3f); // in a stolen car
			if (std::exchange(g_easy, false))
				p.nerve = 0.05f, p.hostility = 0.05f, p.guilt = 0.72f, p.armed = 2, p.substanceGrade = 2, p.impairment = ai::Impairment::Substance;
			const int id = suspect::SpawnDriver({me.x + std::cos(a) * r, me.y + std::sin(a) * r, me.z}, p);
			if (!id)
			{
				g_nextCall = ml::TickMs() + 20000;
				return;
			}
			const suspect::Status st = suspect::StatusOf(id);
			const Vehicle car = PED::GET_VEHICLE_PED_IS_IN(st.ped, false);
			g_carName = Text(VEHICLE::GET_DISPLAY_NAME_FROM_VEHICLE_MODEL(ENTITY::GET_ENTITY_MODEL(car)));
			if (g_carName.empty())
				g_carName = "轎車";
			g_car = car;
			g_suspect = id;
			g_call = Call::Offered;
			g_offeredAt = ml::TickMs();
			Radio();
			const Vector3 at = ENTITY::GET_ENTITY_COORDS(st.ped, true);
			g_dropOff = {at, {}, 0, 0, false};
			NextDropOff();
			ml::Notify("調度：{}有一台失竊的{}，車主剛報案。按 F9「派遣」接受或忽略。", Zone(at), g_carName);
			ml::Log("dispatch: offered a stolen car ({}) at ({:.0f}, {:.0f})", g_carName, at.x, at.y);
		}

		// Where the car was last reported: an area around it, not the car itself, until the officer has found it.
		void Report(bool first)
		{
			const suspect::Status st = suspect::StatusOf(g_suspect);
			const bool car = g_car && ENTITY::DOES_ENTITY_EXIST(g_car);
			if (!car && st.gone)
				return;
			const Vector3 at = ENTITY::GET_ENTITY_COORDS(car ? g_car : st.ped, true);
			const float off = 60.0f;
			const Vector3 c = {at.x + (ai::Random() - 0.5f) * off, at.y + (ai::Random() - 0.5f) * off, at.z};
			ClearArea();
			g_area = HUD::ADD_BLIP_FOR_RADIUS(c.x, c.y, c.z, 150.0f);
			HUD::SET_BLIP_COLOUR(g_area, 5);
			HUD::SET_BLIP_ALPHA(g_area, 110);
			HUD::SET_BLIP_ROUTE(g_area, true);
			HUD::SET_BLIP_ROUTE_COLOUR(g_area, 5);
			if (!first)
			{
				const Vector3 v = car ? ENTITY::GET_ENTITY_VELOCITY(g_car) : Vector3{};
				if (std::sqrt(v.x * v.x + v.y * v.y) > 2.0f)
					ml::Notify("調度：有民眾看到那台{}在{}，往{}邊開。", g_carName, Zone(at), Compass(v.x, v.y));
				else if (car && VEHICLE::IS_VEHICLE_SEAT_FREE(g_car, -1, false))
					ml::Notify("調度：有民眾看到那台{}停在{}路邊。", g_carName, Zone(at));
				else
					ml::Notify("調度：有民眾看到那台{}在{}。", g_carName, Zone(at));
			}
		}

		void Accept()
		{
			if (g_call != Call::Offered)
				return;
			g_call = Call::Active;
			g_found = false;
			g_acceptedAt = ml::TickMs();
			Radio();
			ml::Notify("收到，前往處理。找到後打開警燈攔停。");
			Report(true);
			g_reportAt = ml::TickMs() + 20000;
			records::MarkStolen(g_car);
			records::BeginCase("失竊車輛（" + g_carName + "，車牌 " + records::VehicleOf(g_car).plate + "）", Zone(ENTITY::GET_ENTITY_COORDS(g_car, true)));
			records::Charge("竊盜（車輛）");
			ml::Log("dispatch: accepted");
		}

		void Ignore()
		{
			if (g_call != Call::Offered)
				return;
			suspect::Release(g_suspect);
			End("已轉給其他單位");
		}

		// ---- the action menu
		int g_holdSuspect = 0;
		// A suspect waiting for the backup's car to come up (handed over to them).
		struct HandOver
		{
			int id = 0;
			Vehicle car = 0;
			uint64_t since = 0;
		} g_handOver;
		uint64_t g_holdSince = 0;

		Vehicle PatrolCarNear(const Vector3& at)
		{
			// The officer's patrol car, the car they last drove, the backup's.
			std::vector<Vehicle> cars = units::Cars();
			cars.insert(cars.begin(), PED::GET_VEHICLE_PED_IS_IN(PLAYER::PLAYER_PED_ID(), true));
			cars.insert(cars.begin(), duty::PatrolCar());
			for (const Vehicle v : cars)
				if (v && ENTITY::DOES_ENTITY_EXIST(v) && VEHICLE::GET_VEHICLE_CLASS(v) == 18 && Distance(ENTITY::GET_ENTITY_COORDS(v, true), at) < 60.0f)
					return v;
			// Any other police car around.
			int64_t buf[1 + 24] = {24};
			const int n = PED::GET_PED_NEARBY_VEHICLES(PLAYER::PLAYER_PED_ID(), reinterpret_cast<Any*>(buf));
			for (int i = 0; i < n && i < 24; ++i)
				if (const Vehicle v = static_cast<Vehicle>(buf[1 + i]);
				    v && VEHICLE::GET_VEHICLE_CLASS(v) == 18 && Distance(ENTITY::GET_ENTITY_COORDS(v, true), at) < 60.0f && !suspect::OwnerOf(v))
					return v;
			return 0;
		}

		// When there is none within 60 m: how far off the officer's own car is (walked to, a cuffed suspect follows).
		std::string NoPatrolCar(const Vector3& at)
		{
			for (const Vehicle v : {duty::PatrolCar(), PED::GET_VEHICLE_PED_IS_IN(PLAYER::PLAYER_PED_ID(), true)})
				if (v && ENTITY::DOES_ENTITY_EXIST(v) && VEHICLE::GET_VEHICLE_CLASS(v) == 18)
					return "警車在 " + std::to_string(static_cast<int>(Distance(ENTITY::GET_ENTITY_COORDS(v, true), at))) + " 公尺外";
			return "附近沒有警車";
		}

		// The nearest backup car (theirs to drive) within 60 m.
		Vehicle UnitCarNear(const Vector3& at)
		{
			Vehicle best = 0;
			float bestD = 60.0f;
			for (const Vehicle v : units::Cars())
				if (const float d = Distance(ENTITY::GET_ENTITY_COORDS(v, true), at); d < bestD && !ENTITY::IS_ENTITY_DEAD(v, false))
					best = v, bestD = d;
			return best;
		}

		// Whether a backup officer is at hand to help ("" = yes, else why not).
		std::string HelpState(const Vector3& at)
		{
			if (!units::Backup())
				return "沒有支援";
			return units::Near(at, 120.0f) > 0 ? "" : "支援還沒到";
		}

		// The vehicle search areas: each one, or all that are left, by the officer or the backup.
		menu::List CarSearch(Vehicle car)
		{
			menu::List m{"搜車", {}};
			for (int z = 0; z < search::kZones; ++z)
			{
				menu::Item i{search::ZoneName(z), search::Searched(car, z) ? "已搜" : "", "走到車門搜查這一區"};
				i.action = [car, z] { search::SearchCar(car, z, false); };
				m.items.push_back(i);
			}
			menu::Item all{"全部（剩下的）", "", "依序搜完沒搜過的區域"};
			all.action = [car] { search::SearchCar(car, -1, false); };
			m.items.push_back(all);
			menu::Item help{"請支援搜車", HelpState(ENTITY::GET_ENTITY_COORDS(car, true)), "身邊的支援警員去搜剩下的區域，完成後無線電回報"};
			help.enabled = help.detail.empty();
			help.action = [car] { search::SearchCar(car, -1, true); };
			m.items.push_back(help);
			return m;
		}

		menu::List Actions(int id)
		{
			const suspect::Status st = suspect::StatusOf(id);
			const ::Ped ped = st.ped;
			const Vehicle car = suspect::CarOf(id);
			menu::List m{"處理嫌犯", {}};
			if (st.inVehicle)
			{
				menu::Item out{"叫他下車", "", "熄火、下車"};
				out.action = [id] { suspect::OrderOut(id); };
				m.items.push_back(out);
			}
			menu::Item idc{"盤查證件", "", "核對證件（結果存進車上電腦）"};
			idc.action = [ped] {
				const records::Person& p = records::PersonOf(ped);
				records::Checked(ped);
				records::Involved(p.name + "（嫌犯）");
				records::Note("盤查嫌犯證件：" + p.name);
				ml::Notify("證件：{}，{}。駕照：{}", p.name, p.dob, p.licence);
			};
			m.items.push_back(idc);
			menu::Item radio{"向調度查詢此人", "", "通緝、前科（無線電回覆）"};
			radio.action = [ped] { mdt::RadioPerson(ped); };
			m.items.push_back(radio);
			if (car)
			{
				menu::Item plate{"查車牌", "", "車主、失竊、行照保險（無線電回覆）"};
				plate.action = [car] { mdt::RadioPlate(car); };
				m.items.push_back(plate);
			}
			if (!st.inVehicle)
			{
				menu::Item frisk{"搜身", search::Frisked(ped) ? "已搜" : "", "武器、管制藥物"};
				frisk.action = [ped] { search::Frisk(ped, false); };
				m.items.push_back(frisk);
				menu::Item friskHelp{"請支援搜身", HelpState(ENTITY::GET_ENTITY_COORDS(ped, true)), "身邊的支援警員來搜"};
				friskHelp.enabled = friskHelp.detail.empty();
				friskHelp.action = [ped] { search::Frisk(ped, true); };
				m.items.push_back(friskHelp);
				if (car && !PED::IS_PED_IN_VEHICLE(ped, car, false))
				{
					menu::Item sc{"搜車", "", "分區搜查嫌犯的車"};
					sc.sub = [car] { return CarSearch(car); };
					m.items.push_back(sc);
				}
				if (!st.cuffed)
				{
					menu::Item cuff{"上手銬", "", "逮捕"};
					cuff.action = [id] {
						if (suspect::Cuff(id))
							ml::Notify("已上手銬");
					};
					m.items.push_back(cuff);
				}
				else
				{
					const Vector3 me = ENTITY::GET_ENTITY_COORDS(PLAYER::PLAYER_PED_ID(), true);
					menu::Item put{"押上警車（自己載送）", "", "60 公尺內的警車，之後開去警局或監獄"};
					put.enabled = PatrolCarNear(me) != 0;
					if (!put.enabled)
						put.detail = NoPatrolCar(me);
					put.action = [id] { suspect::PutInCar(id, PatrolCarNear(ENTITY::GET_ENTITY_COORDS(PLAYER::PLAYER_PED_ID(), true))); };
					m.items.push_back(put);
					const Vehicle unit = UnitCarNear(ENTITY::GET_ENTITY_COORDS(PLAYER::PLAYER_PED_ID(), true));
					const bool fetchable = !unit && units::Near(ENTITY::GET_ENTITY_COORDS(ped, true), 400.0f) > 0;
					menu::Item hand{"交給支援載送", unit ? "" : fetchable ? "支援開車過來" : units::Backup() ? "支援還沒到" : "沒有支援",
					    unit ? "押上支援的警車，由他們送去收押" : "支援把警車開過來，再押上車由他們送去收押"};
					hand.enabled = unit != 0 || fetchable;
					hand.action = [id, unit] {
						custody::ByBackup(id);
						if (unit)
							suspect::PutInCar(id, unit);
						else if (const Vehicle coming = units::Summon(ENTITY::GET_ENTITY_COORDS(suspect::StatusOf(id).ped, true)))
						{
							g_handOver = {id, coming, ml::TickMs()};
							ml::Notify("支援：收到，車開過來了");
						}
					};
					m.items.push_back(hand);
				}
			}
			menu::Item go{"放他走", "", "不予逮捕"};
			go.action = [id] {
				suspect::Release(id);
				records::Note("嫌犯經盤查後放行");
				ml::Notify("已放行");
			};
			m.items.push_back(go);
			return m;
		}

		void TickActions()
		{
			const ::Ped player = PLAYER::PLAYER_PED_ID();
			if (menu::IsOpen() || PED::IS_PED_IN_ANY_VEHICLE(player, false) || !duty::OnDuty() || custody::Busy())
			{
				g_holdSuspect = 0;
				return;
			}
			const Vector3 me = ENTITY::GET_ENTITY_COORDS(player, true);
			if (search::Busy())
				return;
			const int id = suspect::Within(me, 3.0f);
			if (!id)
			{
				// Next to a suspect's vehicle they are out of: search it.
				const Vehicle v = VEHICLE::GET_CLOSEST_VEHICLE(me.x, me.y, me.z, 4.0f, 0, 70);
				const ::Ped owner = v ? suspect::OwnerOf(v) : 0;
				if (owner && !PED::IS_PED_IN_VEHICLE(owner, v, false))
				{
					HUD::BEGIN_TEXT_COMMAND_DISPLAY_HELP("STRING");
					HUD::ADD_TEXT_COMPONENT_SUBSTRING_PLAYER_NAME("按住 ~INPUT_CONTEXT~ 搜車");
					HUD::END_TEXT_COMMAND_DISPLAY_HELP(0, false, false, -1);
					if (PAD::IS_CONTROL_PRESSED(0, 51))
					{
						if (g_holdSuspect != -1)
							g_holdSuspect = -1, g_holdSince = ml::TickMs();
						else if (ml::TickMs() - g_holdSince > 450)
						{
							g_holdSuspect = 0;
							menu::Open(CarSearch(v));
						}
					}
					else
						g_holdSuspect = 0;
					return;
				}
				g_holdSuspect = 0;
				return;
			}
			const suspect::Status st = suspect::StatusOf(id);
			if (!(st.yielded || st.cuffed) || st.dead)
				return;
			HUD::BEGIN_TEXT_COMMAND_DISPLAY_HELP("STRING");
			HUD::ADD_TEXT_COMPONENT_SUBSTRING_PLAYER_NAME("按住 ~INPUT_CONTEXT~ 處理嫌犯");
			HUD::END_TEXT_COMMAND_DISPLAY_HELP(0, false, false, -1);
			if (PAD::IS_CONTROL_PRESSED(0, 51))
			{
				if (g_holdSuspect != id)
				{
					g_holdSuspect = id;
					g_holdSince = ml::TickMs();
				}
				else if (ml::TickMs() - g_holdSince > 450)
				{
					g_holdSuspect = 0;
					menu::Open(Actions(id));
				}
			}
			else
				g_holdSuspect = 0;
		}
	}

	menu::Item MenuItem()
	{
		if (g_call == Call::Offered)
		{
			menu::Item i{"派遣", "失竊車輛", "有新的報案"};
			i.sub = [] {
				menu::List m{"派遣：失竊車輛", {}};
				menu::Item yes{"接受", "", "前往處理"};
				yes.action = Accept;
				m.items.push_back(yes);
				menu::Item no{"忽略", "", "交給其他單位"};
				no.action = Ignore;
				m.items.push_back(no);
				return m;
			};
			return i;
		}
		if (g_call == Call::Active)
		{
			menu::Item i{"派遣", "處理中", "目前的案件"};
			i.sub = [] {
				menu::List m{"派遣：失竊車輛", {}};
				menu::Item quit{"放棄案件", "", "嫌犯交給其他單位"};
				quit.action = [] {
					suspect::Release(g_suspect);
					End("案件已轉出");
				};
				m.items.push_back(quit);
				return m;
			};
			return i;
		}
		menu::Item i{"派遣", "", "目前沒有報案"};
		i.enabled = false;
		return i;
	}

	void CallNow(bool easy)
	{
		g_easy = easy;
		if (g_call == Call::None)
			g_nextCall = 1; // due
	}

	void Forget()
	{
		g_handOver = {};
		g_call = Call::None;
		g_suspect = 0;
		g_area = 0;
		g_car = 0;
		g_dropOff.done = true;
	}

	void Tick()
	{
		const uint64_t now = ml::TickMs();
		if (!officer::IsPlayer())
		{
			if (g_call != Call::None)
			{
				suspect::Release(g_suspect);
				ClearArea();
				LetCarGo();
				g_call = Call::None;
			}
			return;
		}
		TickActions();
		if (g_handOver.id)
		{
			const suspect::Status st = suspect::StatusOf(g_handOver.id);
			const bool there = ENTITY::DOES_ENTITY_EXIST(g_handOver.car) &&
			                   Distance(ENTITY::GET_ENTITY_COORDS(g_handOver.car, true), ENTITY::GET_ENTITY_COORDS(st.ped, true)) < 20.0f;
			if (st.gone || !st.cuffed)
				g_handOver = {};
			else if (there)
			{
				suspect::PutInCar(g_handOver.id, g_handOver.car);
				g_handOver = {};
			}
			else if (now - g_handOver.since > 120000)
			{
				ml::Notify("支援的車過不來：押上你的警車");
				g_handOver = {};
			}
		}
		PlanDropOff();
		switch (g_call)
		{
		case Call::None:
			if (!g_nextCall)
				g_nextCall = now + 120000 + static_cast<uint64_t>(ai::Random() * 180000.0f); // 2-5 minutes on duty
			if (duty::OnDuty() && now >= g_nextCall && suspect::Count() == 0)
				Offer();
			break;
		case Call::Offered:
			if (now - g_offeredAt > kOfferTime)
			{
				suspect::Release(g_suspect);
				End("沒有回應，已轉給其他單位");
			}
			break;
		case Call::Active:
		{
			const suspect::Status st = suspect::StatusOf(g_suspect);
			const ::Ped player = PLAYER::PLAYER_PED_ID();
			const bool car = g_car && ENTITY::DOES_ENTITY_EXIST(g_car);
			if (car && !ENTITY::IS_ENTITY_A_MISSION_ENTITY(g_car))
				ENTITY::SET_ENTITY_AS_MISSION_ENTITY(g_car, true, true); // kept when the thief is gone (until the call ends)
			if (std::string where; custody::Booked(g_suspect, &where))
			{
				End(("嫌犯已收押於" + where + "，案件結束。辛苦了。").c_str(), ("嫌犯逮捕，收押於" + where).c_str());
				break;
			}
			if (st.dead)
			{
				End("嫌犯死亡，案件結束", "嫌犯死亡");
				break;
			}
			// Found: the officer sees the car (the thief at the wheel or not) or the thief being pursued, or the plate
			// reader reads it (the suspect's own blip shows from now on).
			const bool wasFound = g_found;
			suspect::Target t;
			if (!g_found && !st.gone && suspect::Pursued(t) && t.known)
				g_found = true;
			if (!g_found && mdt::Ahead() && (mdt::Ahead() == g_car || mdt::Ahead() == suspect::CarOf(g_suspect)))
				g_found = true;
			if (!g_found && (car || !st.gone))
			{
				const Entity seen = car ? g_car : st.ped;
				if (Distance(ENTITY::GET_ENTITY_COORDS(player, true), ENTITY::GET_ENTITY_COORDS(seen, true)) < 90.0f &&
				    ENTITY::HAS_ENTITY_CLEAR_LOS_TO_ENTITY(player, seen, 17))
					g_found = true;
			}
			if (g_found && !wasFound)
			{
				ClearArea();
				const Vector3 at = ENTITY::GET_ENTITY_COORDS(car ? g_car : st.ped, true);
				records::Note("發現失竊車輛（" + Zone(at) + "）");
				if (!st.gone && car && !PED::IS_PED_IN_VEHICLE(st.ped, g_car, false))
				{
					// Left at the roadside: the thief may still be about.
					records::Note("車輛停放路邊，駕駛已離開車輛");
					const Vector3 thief = ENTITY::GET_ENTITY_COORDS(st.ped, true);
					if (Distance(thief, at) > 8.0f)
						ml::Notify("找到那台{}了：停在路邊，車上沒人。有民眾看到駕駛下車往{}邊走，可能還在附近。", g_carName, Compass(thief.x - at.x, thief.y - at.y));
					else
						ml::Notify("找到那台{}了：停在路邊，駕駛就在旁邊。", g_carName);
				}
				else if (!st.gone)
					ml::Notify("找到那台{}了", g_carName);
			}
			if (st.gone)
			{
				// Away at the wheel, the car with them; left somewhere, the car recovered once found (until then, still to be
				// found).
				const ::Ped driver = car ? VEHICLE::GET_PED_IN_VEHICLE_SEAT(g_car, -1, false) : 0;
				if (!car || (driver && driver != player))
				{
					End("嫌犯逃脫");
					break;
				}
				if (g_found)
				{
					if (wasFound)
						End("嫌犯逃脫，失竊車輛已尋獲", "嫌犯逃脫，尋獲失竊車輛");
					else
						End("失竊車輛已尋獲，竊嫌已經離開", "尋獲失竊車輛，竊嫌不明");
					break;
				}
			}
			if (!g_found && now - g_acceptedAt > kLostAfter)
			{
				if (!st.gone)
					suspect::Release(g_suspect);
				End(("一直沒有找到那台" + g_carName + "，失去蹤跡，轉為協尋").c_str(), "失去蹤跡，車輛未尋獲");
				break;
			}
			if (!g_found && now >= g_reportAt)
			{
				Report(false);
				g_reportAt = now + 25000;
			}
			break;
		}
		}
	}
}
