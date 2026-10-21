// Taking a suspect in (user's design): once in a police car, the suspect is driven to a station's holding cells or,
// on serious charges (a weapon, attacking the police, a carjacking, grade 1-2 controlled substances, a warrant), to
// Bolingbroke prison; a station books those too and has them transferred. The officer drives them in, or hands them to
// the backup, who drive them there themselves.
//
// On the way the suspect talks (pleads, curses or says nothing, as they are), and given the chance (the rear door open
// or broken, the car wrecked, the officer far away) a daring one breaks out and runs, cuffed. How the officer drives
// (collisions, speeding, the siren on a transport that is no emergency) goes into the report's score.
//
// At the facility a jailer waits at the door. Booking is done there step by step (photograph, fingerprints, property,
// charges), each taking a few seconds; the jailer then takes the suspect in, and the case closes.
#include "police.hpp"

#include <algorithm>
#include <cmath>
#include <map>
#include <string>
#include <vector>

namespace police::custody
{
	namespace
	{
		using ai::Distance;
		using ai::Random;

		struct Facility
		{
			const char* name;
			Place park; // where the car stops
			Place door; // where the jailer waits (facing out)
			bool prison;
		};
		const Facility kFacilities[] = {
		    {"Mission Row 警局", {443.0f, -1019.0f, 28.5f, 90.0f}, {450.0f, -1012.6f, 28.49f, 180.0f}, false},     // the rear lot, the ramp's shutters
		    {"La Mesa 警局", {832.0f, -1270.5f, 26.3f, 0.0f}, {823.6f, -1290.0f, 29.3f, 90.0f}, false},            // the lot, the landing up the steps
		    {"Vespucci 警局", {-1095.0f, -788.0f, 19.2f, 0.0f}, {-1092.0f, -801.0f, 20.3f, 0.0f}, false},          // the plaza, the front doors
		    {"Davis 警長辦公室", {381.0f, -1570.0f, 29.3f, 0.0f}, {366.0f, -1577.0f, 29.3f, 305.0f}, false},       // Davis Ave, the side walk in
		    {"Sandy Shores 警長辦公室", {1855.8f, 3671.9f, 33.5f, 120.0f}, {1855.6f, 3682.0f, 34.3f, 210.0f}, false}, // the yard, the porch
		    {"Paleto Bay 警長辦公室", {-420.0f, 6013.0f, 31.3f, 0.0f}, {-437.8f, 6017.8f, 31.5f, 300.0f}, false},   // the road, the front door
		    {"Bolingbroke 監獄", {1859.0f, 2605.0f, 45.7f, 0.0f}, {1850.5f, 2602.0f, 45.67f, 270.0f}, true},       // the lot, inmate receiving gate 1
		};
		constexpr int kCount = static_cast<int>(std::size(kFacilities));

		enum class Stage
		{
			None,
			Transport, // on the way
			Arrived,   // at the door: booking
			Leaving,   // the jailer takes them in
		};

		struct Step
		{
			const char* name;
			const char* scenario; // what the officer does meanwhile
			uint64_t ms;
		};
		const Step kSteps[] = {
		    {"拍照建檔", "WORLD_HUMAN_PAPARAZZI", 4000},
		    {"採集指紋", "CODE_HUMAN_POLICE_INVESTIGATE", 4000},
		    {"清點保管財物", "CODE_HUMAN_POLICE_INVESTIGATE", 3500},
		    {"登錄罪名", "WORLD_HUMAN_CLIPBOARD", 4000},
		};
		constexpr int kStepCount = static_cast<int>(std::size(kSteps));

		struct Job
		{
			int id = 0;
			::Ped ped = 0;
			Vehicle car = 0;
			int dest = 0;
			Stage stage = Stage::None;
			bool backup = false; // the backup drives them
			bool felony = false;
			Blip blip = 0;
			::Ped jailer = 0;
			bool refused = false; // the prison turned a minor case away (said once)
			// Booking.
			int step = 0;          // steps done
			int running = -1;      // the step under way
			bool all = false;      // the remaining steps one after another
			uint64_t stepUntil = 0, stepStarted = 0, leaveAt = 0;
			// On the way.
			uint64_t begun = 0;
			uint64_t talkAt = 0, sampleAt = 0, speedSince = 0, sirenSince = 0, speedNoted = 0, crashNoted = 0, awaySince = 0, escapeAt = 0;
			float health = 0;
			int penalty = 0;      // driving points lost (capped)
			bool sirenNoted = false;
		};
		Job g_job;
		std::vector<int> g_byBackup;         // suspects to be handed to the backup
		std::map<int, std::string> g_booked; // suspect id -> where
		int g_escaped = 0;                   // the suspect who broke out on the way
		int g_hold = 0;
		uint64_t g_holdSince = 0;

		Vector3 V(const Place& p)
		{
			return {p.x, p.y, p.z};
		}

		// Serious enough for the prison.
		bool Felony(::Ped ped)
		{
			static const char* const serious[] = {"以武器攻擊警察", "強盜（劫車）", "非法持有武器", "持有第一級管制藥物", "持有第二級管制藥物", "脫逃"};
			for (const std::string& c : records::Charges())
				if (std::find(std::begin(serious), std::end(serious), c) != std::end(serious) || c.rfind("通緝犯", 0) == 0)
					return true;
			return records::PersonOf(ped).warrant;
		}

		int Nearest(const Vector3& at, bool prison)
		{
			int best = 0;
			float bestD = 1e9f;
			for (int i = 0; i < kCount; ++i)
				if (kFacilities[i].prison == prison)
					if (const float d = Distance(at, V(kFacilities[i].park)); d < bestD)
						best = i, bestD = d;
			return best;
		}

		void Subtitle(const std::string& line)
		{
			HUD::BEGIN_TEXT_COMMAND_PRINT("STRING");
			HUD::ADD_TEXT_COMPONENT_SUBSTRING_PLAYER_NAME(("~o~嫌犯：~s~" + line).c_str());
			HUD::END_TEXT_COMMAND_PRINT(5000, true);
		}

		void ClearBlip()
		{
			if (g_job.blip && HUD::DOES_BLIP_EXIST(g_job.blip))
				HUD::REMOVE_BLIP(&g_job.blip);
			g_job.blip = 0;
		}

		void Route()
		{
			ClearBlip();
			const Facility& f = kFacilities[g_job.dest];
			g_job.blip = HUD::ADD_BLIP_FOR_COORD(f.park.x, f.park.y, f.park.z);
			HUD::SET_BLIP_SPRITE(g_job.blip, f.prison ? 188 : 60);
			HUD::SET_BLIP_COLOUR(g_job.blip, 3);
			HUD::SET_BLIP_ROUTE(g_job.blip, true);
			HUD::SET_BLIP_ROUTE_COLOUR(g_job.blip, 3);
			HUD::BEGIN_TEXT_COMMAND_SET_BLIP_NAME("STRING");
			HUD::ADD_TEXT_COMPONENT_SUBSTRING_PLAYER_NAME(f.name);
			HUD::END_TEXT_COMMAND_SET_BLIP_NAME(g_job.blip);
		}

		void DropJailer()
		{
			if (g_job.jailer && ENTITY::DOES_ENTITY_EXIST(g_job.jailer))
			{
				ENTITY::SET_ENTITY_AS_MISSION_ENTITY(g_job.jailer, true, true);
				PED::DELETE_PED(&g_job.jailer);
			}
			g_job.jailer = 0;
		}

		void Reset()
		{
			ClearBlip();
			DropJailer();
			g_job = {};
		}

		// ---- on the way

		// What they say, as they are.
		void Talk(const ai::Profile& p)
		{
			static const char* const pleading[] = {"警官，可以通融一下嗎？我保證不會再犯了。", "我真的不知道會這樣……", "我家裡還有小孩要照顧，拜託。",
			    "能不能讓我打個電話？", "我明天還要上班，會被開除的。"};
			static const char* const hostile[] = {"你們條子就只會欺負人。", "手銬太緊了！你是故意的吧？", "我要找律師，其他什麼都不說。",
			    "你給我記住，我會投訴你。", "抓我有什麼用？真正的壞人你們都不抓。"};
			static const char* const silent[] = {"……", "……我什麼都不會說。", "等我律師來再說。"};
			static const char* const drunk[] = {"我、我只喝了兩杯而已……", "開慢一點，我想吐……", "警官你人很好欸，我們做朋友好不好……"};
			static const char* const high[] = {"頭好暈……有水嗎？", "車子怎麼一直在轉……", "我心跳好快，好不舒服……"};
			static const char* const innocent[] = {"我什麼都沒做，你們抓錯人了。", "這是誤會，真的是誤會。", "我可以解釋的。"};
			const auto pick = [](const auto& list) { return std::string(list[MISC::GET_RANDOM_INT_IN_RANGE(0, static_cast<int>(std::size(list)))]); };
			std::string line;
			if (p.impairment == ai::Impairment::Drunk)
				line = pick(drunk);
			else if (p.impairment == ai::Impairment::Substance && Random() < 0.5f)
				line = pick(high);
			else if (p.hostility > 0.55f)
			{
				line = pick(hostile);
				AUDIO::PLAY_PED_AMBIENT_SPEECH_NATIVE(g_job.ped, "GENERIC_CURSE_MED", "SPEECH_PARAMS_FORCE_NORMAL", 0);
			}
			else if (p.skill == ai::Skill::Veteran)
				line = pick(silent);
			else if (p.guilt > 0.6f)
				line = pick(pleading);
			else
				line = pick(innocent);
			Subtitle(line);
		}

		void Penalty(int points, const std::string& why, const char* says)
		{
			const int left = 20 + g_job.penalty; // at most 20 points for the driving
			points = std::max(points, -left);
			if (points < 0)
			{
				g_job.penalty += points;
				records::Conduct(points, why);
			}
			if (says)
				Subtitle(says);
		}

		// The officer drives them: collisions, speed, the siren.
		void Driving(uint64_t now)
		{
			const Vehicle car = g_job.car;
			if (now < g_job.sampleAt)
				return;
			g_job.sampleAt = now + 250;
			const float health = VEHICLE::GET_VEHICLE_BODY_HEALTH(car);
			const float speed = ENTITY::GET_ENTITY_SPEED(car);
			if (g_job.health > 0 && g_job.health - health > 25.0f && now - g_job.crashNoted > 4000)
			{
				g_job.crashNoted = now;
				Penalty(-3, "載送途中發生碰撞", "喂！你到底會不會開車？");
			}
			g_job.health = health;
			if (speed > 33.0f) // ~120 km/h
			{
				if (!g_job.speedSince)
					g_job.speedSince = now;
				if (now - g_job.speedSince > 3000 && now - g_job.speedNoted > 20000)
				{
					g_job.speedNoted = now;
					Penalty(-2, "載送途中超速", "開這麼快是要去投胎喔？");
				}
			}
			else
				g_job.speedSince = 0;
			if (lights::Stage(car) >= 3)
			{
				if (!g_job.sirenSince)
					g_job.sirenSince = now;
				if (now - g_job.sirenSince > 5000 && !g_job.sirenNoted)
				{
					g_job.sirenNoted = true;
					Penalty(-5, "非緊急載送使用警笛", nullptr);
					ml::Notify("載送嫌犯不是緊急勤務，不需要開警笛");
				}
			}
			else
				g_job.sirenSince = 0;
		}

		// Left alone with a way out: a daring suspect takes it.
		void Chance(uint64_t now, const ai::Profile& p)
		{
			const Vehicle car = g_job.car;
			const ::Ped player = PLAYER::PLAYER_PED_ID();
			const Vector3 at = ENTITY::GET_ENTITY_COORDS(car, true);
			const float officer = Distance(ENTITY::GET_ENTITY_COORDS(player, true), at);
			if (officer > 35.0f)
			{
				if (!g_job.awaySince)
					g_job.awaySince = now;
			}
			else
				g_job.awaySince = 0;
			const int door = VEHICLE::GET_PED_IN_VEHICLE_SEAT(car, 1, false) == g_job.ped ? 2 : 3;
			const bool doorOpen = VEHICLE::GET_VEHICLE_DOOR_ANGLE_RATIO(car, door) > 0.2f || VEHICLE::IS_VEHICLE_DOOR_DAMAGED(car, door);
			const bool wrecked = VEHICLE::GET_VEHICLE_ENGINE_HEALTH(car) < 0.0f || FIRE::IS_ENTITY_ON_FIRE(car) || ENTITY::IS_ENTITY_UPSIDEDOWN(car);
			const bool alone = g_job.awaySince && now - g_job.awaySince > 20000;
			// (the door they were put in through is still closing for a moment)
			if (!(doorOpen || wrecked || alone) || PED::IS_PED_IN_VEHICLE(player, car, false) || now - g_job.begun < 8000)
				return;
			if (now < g_job.escapeAt)
				return;
			g_job.escapeAt = now + 4000;
			float dare = p.nerve * 0.5f + p.guilt * 0.3f + (wrecked ? 0.3f : 0.0f) - (p.impairment == ai::Impairment::Drunk ? 0.1f : 0.0f);
			if (units::Near(at, 15.0f) > 0 || officer < 6.0f)
				dare -= 0.4f; // someone right there
			const float roll = Random();
			ml::Log("custody: a way out (door {} wrecked {} alone {}), dare {:.2f} roll {:.2f}", doorOpen, wrecked, alone, dare, roll);
			if (roll < dare * 0.35f || (wrecked && FIRE::IS_ENTITY_ON_FIRE(car)))
			{
				g_escaped = g_job.id;
				suspect::Escape(g_job.id);
				records::Conduct(-15, "載送途中嫌犯脫逃");
				ml::Notify("~r~嫌犯從警車脫逃了！");
				ClearBlip();
				g_job.stage = Stage::None; // back in a car, the transport begins anew
			}
		}

		// ---- at the door

		void Arrive()
		{
			const Facility& f = kFacilities[g_job.dest];
			ClearBlip();
			suspect::TakeOut(g_job.id);
			// The jailer at the door.
			const Hash model = MISC::GET_HASH_KEY(f.prison ? "s_m_m_prisguard_01" : duty::CopModel());
			if (const auto loaded = ml::LoadModel(model))
			{
				g_job.jailer = PED::CREATE_PED(6, model, f.door.x, f.door.y, f.door.z - 1.0f, f.door.heading, false, false);
				if (g_job.jailer)
				{
					ENTITY::SET_ENTITY_AS_MISSION_ENTITY(g_job.jailer, true, true);
					PED::SET_PED_RANDOM_COMPONENT_VARIATION(g_job.jailer, 0);
					ai::TakeControl(g_job.jailer);
					TASK::TASK_START_SCENARIO_IN_PLACE(g_job.jailer, "WORLD_HUMAN_COP_IDLES", 0, true);
				}
			}
			g_job.stage = Stage::Arrived;
			records::Note("抵達" + std::string(f.name));
			ml::Notify("到了。帶嫌犯到門口，按住 ~INPUT_CONTEXT~ 辦理收押");
			ml::Log("custody: arrived at {}", f.name);
		}

		void Record(const char* how)
		{
			const Facility& f = kFacilities[g_job.dest];
			g_booked[g_job.id] = f.name;
			records::Note(std::string("收押完成（") + f.name + "）" + how);
			if (g_job.felony && !f.prison)
				records::Note("重罪嫌犯，收押後移送 Bolingbroke 監獄");
			ml::Log("custody: suspect {} booked at {}", g_job.id, f.name);
		}

		void Finish()
		{
			// Clean driving, nobody got away: it shows.
			if (!g_job.backup && g_job.penalty == 0 && g_escaped != g_job.id)
				records::Conduct(5, "載送平穩守規");
			Record("");
			// Out of view while they go in.
			CAMERA::DO_SCREEN_FADE_OUT(400);
			ml::Wait(500);
			suspect::Delete(g_job.id);
			DropJailer();
			CAMERA::DO_SCREEN_FADE_IN(600);
			ml::Notify("收押完成：嫌犯已交給{}", kFacilities[g_job.dest].prison ? "獄警" : "拘留室");
			Reset();
		}

		void StartStep(int i);

		menu::List Booking()
		{
			menu::List m{std::string("收押：") + kFacilities[g_job.dest].name, {}};
			for (int i = 0; i < kStepCount; ++i)
			{
				menu::Item it{kSteps[i].name, i < g_job.step ? "完成" : "", i == g_job.step ? "做這一步" : i < g_job.step ? "" : "要先完成前面的步驟"};
				it.enabled = i == g_job.step;
				it.action = [i] { StartStep(i); };
				m.items.push_back(it);
			}
			menu::Item all{"依序完成剩下的", "", "一步接一步做完"};
			all.action = [] {
				g_job.all = true;
				StartStep(g_job.step);
			};
			m.items.push_back(all);
			return m;
		}

		void StartStep(int i)
		{
			if (g_job.stage != Stage::Arrived || i != g_job.step || i >= kStepCount)
				return;
			const ::Ped me = PLAYER::PLAYER_PED_ID();
			const Vector3 at = ENTITY::GET_ENTITY_COORDS(g_job.ped, true);
			const Vector3 mine = ENTITY::GET_ENTITY_COORDS(me, true);
			ENTITY::SET_ENTITY_HEADING(me, MISC::GET_HEADING_FROM_VECTOR_2D(at.x - mine.x, at.y - mine.y));
			TASK::TASK_START_SCENARIO_IN_PLACE(me, kSteps[i].scenario, 0, true);
			g_job.running = i;
			g_job.stepStarted = ml::TickMs();
			g_job.stepUntil = g_job.stepStarted + kSteps[i].ms;
		}

		void EndStep(int i)
		{
			const ::Ped ped = g_job.ped;
			switch (i)
			{
			case 0:
				AUDIO::PLAY_SOUND_FRONTEND(-1, "Camera_Shoot", "Phone_Soundset_Franklin", true);
				records::Note("收押：拍照建檔");
				ml::Notify("拍照建檔完成");
				break;
			case 1:
			{
				records::Person& p = records::PersonOf(ped);
				records::Checked(ped);
				records::Involved(p.name + "（嫌犯）");
				records::Note("收押：採集指紋，比對身分為 " + p.name);
				if (p.warrant)
				{
					records::Charge("通緝犯（" + p.warrantFor + "）");
					ml::Notify("指紋比對：{}，~r~有通緝（{}）", p.name, p.warrantFor);
				}
				else
					ml::Notify("指紋比對：{}，無通緝", p.name);
				break;
			}
			case 2:
				if (search::Book(ped))
					records::Conduct(-10, "載送前未搜身（收押時才發現武器）");
				break;
			case 3:
			{
				std::string list;
				for (const std::string& c : records::Charges())
					list += (list.empty() ? "" : "、") + c;
				records::Note("收押：登錄罪名（" + (list.empty() ? std::string("無") : list) + "）");
				ml::Notify("罪名登錄完成：{}", list.empty() ? "無" : list);
				break;
			}
			default:
				break;
			}
		}

		void TickBooking(uint64_t now)
		{
			const ::Ped me = PLAYER::PLAYER_PED_ID();
			const Facility& f = kFacilities[g_job.dest];
			const Vector3 door = V(f.door);
			const Vector3 at = ENTITY::GET_ENTITY_COORDS(me, true);
			if (g_job.running >= 0)
			{
				// Walking off stops it.
				if ((std::abs(PAD::GET_CONTROL_NORMAL(0, 30)) > 0.5f || std::abs(PAD::GET_CONTROL_NORMAL(0, 31)) > 0.5f) && now - g_job.stepStarted > 600)
				{
					TASK::CLEAR_PED_TASKS(me);
					g_job.running = -1;
					g_job.all = false;
					ml::Notify("收押中斷");
					return;
				}
				// The countdown.
				const int left = static_cast<int>((g_job.stepUntil > now ? g_job.stepUntil - now : 0) / 1000) + 1;
				HUD::BEGIN_TEXT_COMMAND_DISPLAY_HELP("STRING");
				HUD::ADD_TEXT_COMPONENT_SUBSTRING_PLAYER_NAME((std::string(kSteps[g_job.running].name) + "中… " + std::to_string(left)).c_str());
				HUD::END_TEXT_COMMAND_DISPLAY_HELP(0, false, false, -1);
				if (now < g_job.stepUntil)
					return;
				TASK::CLEAR_PED_TASKS(me);
				EndStep(g_job.running);
				g_job.running = -1;
				++g_job.step;
				if (g_job.step >= kStepCount)
				{
					// The jailer takes them in (towards the door behind them), the suspect following.
					const Vector3 in = {door.x - std::sin(f.door.heading * 0.01745f) * -3.0f, door.y + std::cos(f.door.heading * 0.01745f) * -3.0f, door.z};
					if (g_job.jailer && ENTITY::DOES_ENTITY_EXIST(g_job.jailer))
					{
						TASK::CLEAR_PED_TASKS(g_job.jailer);
						TASK::TASK_GO_STRAIGHT_TO_COORD(g_job.jailer, in.x, in.y, in.z, 1.0f, 8000, f.door.heading + 180.0f, 0.5f);
						suspect::LedBy(g_job.id, g_job.jailer);
					}
					g_job.stage = Stage::Leaving;
					g_job.leaveAt = now + 3500;
					return;
				}
				if (g_job.all)
					StartStep(g_job.step);
				else
					menu::Open(Booking());
				return;
			}
			if (menu::IsOpen())
				return;
			// Both at the door: hold E.
			const bool ready = Distance(at, door) < 4.0f && Distance(ENTITY::GET_ENTITY_COORDS(g_job.ped, true), door) < 7.0f;
			if (!ready)
			{
				GRAPHICS::DRAW_MARKER(1, door.x, door.y, door.z - 1.0f, 0, 0, 0, 0, 0, 0, 1.2f, 1.2f, 0.6f, 93, 182, 229, 120, false, false, 2, false, nullptr, nullptr, false);
				g_hold = 0;
				return;
			}
			HUD::BEGIN_TEXT_COMMAND_DISPLAY_HELP("STRING");
			HUD::ADD_TEXT_COMPONENT_SUBSTRING_PLAYER_NAME("按住 ~INPUT_CONTEXT~ 辦理收押");
			HUD::END_TEXT_COMMAND_DISPLAY_HELP(0, false, false, -1);
			if (PAD::IS_CONTROL_PRESSED(0, 51))
			{
				if (!g_hold)
					g_hold = 1, g_holdSince = now;
				else if (now - g_holdSince > 450)
				{
					g_hold = 0;
					menu::Open(Booking());
				}
			}
			else
				g_hold = 0;
		}
	}

	void ByBackup(int id)
	{
		g_byBackup.push_back(id);
	}

	void Begin(int id, Vehicle car)
	{
		const suspect::Status st = suspect::StatusOf(id);
		Reset();
		g_job.id = id;
		g_job.ped = st.ped;
		g_job.car = car;
		g_job.felony = Felony(st.ped);
		g_job.stage = Stage::Transport;
		g_job.health = VEHICLE::GET_VEHICLE_BODY_HEALTH(car);
		g_job.begun = ml::TickMs();
		// The officer shuts the rear doors behind them.
		for (const int door : {2, 3})
			VEHICLE::SET_VEHICLE_DOOR_SHUT(car, door, false);
		g_job.talkAt = ml::TickMs() + 12000 + static_cast<uint64_t>(Random() * 15000.0f);
		const Vector3 at = ENTITY::GET_ENTITY_COORDS(car, true);
		g_job.dest = Nearest(at, g_job.felony);
		const Facility& f = kFacilities[g_job.dest];
		const auto asked = std::find(g_byBackup.begin(), g_byBackup.end(), id);
		g_job.backup = units::IsUnitCar(car);
		if (asked != g_byBackup.end())
			g_byBackup.erase(asked);
		if (g_job.backup && units::Transport(car, V(f.park)))
		{
			records::Note(std::string("嫌犯交由支援警員載送至") + f.name);
			ml::Notify("支援：收到，嫌犯由我們送去{}", f.name);
		}
		else
		{
			g_job.backup = false;
			// The scene is done: the backup goes back to patrol.
			if (units::Backup())
				units::Clear();
			Route();
			records::Note(std::string("開始載送嫌犯，目的地：") + f.name);
			ml::Notify("載送嫌犯到{}（{}）。F9「載送」可以改目的地。", f.name, g_job.felony ? "重罪，直接移送監獄" : "最近的拘留室");
		}
		ml::Log("custody: suspect {} to {} ({}{})", id, f.name, g_job.felony ? "felony" : "minor", g_job.backup ? ", by the backup" : "");
	}

	bool Busy()
	{
		return g_job.stage == Stage::Arrived || g_job.stage == Stage::Leaving;
	}

	bool Booked(int id, std::string* where)
	{
		const auto it = g_booked.find(id);
		if (it == g_booked.end())
			return false;
		if (where)
			*where = it->second;
		return true;
	}

	menu::Item MenuItem()
	{
		if (g_job.stage == Stage::None)
		{
			menu::Item i{"載送", "", "沒有載送中的嫌犯"};
			i.enabled = false;
			return i;
		}
		menu::Item i{"載送", kFacilities[g_job.dest].name, g_job.backup ? "支援載送中" : "改目的地"};
		i.enabled = !g_job.backup && g_job.stage == Stage::Transport;
		i.sub = [] {
			menu::List m{"載送目的地", {}};
			const Vector3 at = ENTITY::GET_ENTITY_COORDS(g_job.car, true);
			for (int k = 0; k < kCount; ++k)
			{
				const Facility& f = kFacilities[k];
				const int d = static_cast<int>(Distance(at, V(f.park)));
				std::string help = f.prison ? (g_job.felony ? "重罪嫌犯可以直接移送" : "只收重罪嫌犯") : g_job.felony ? "收押後再移送監獄" : "拘留室";
				char dist[32];
				std::snprintf(dist, sizeof dist, d >= 1000 ? "%.1f km" : "%.0f m", d >= 1000 ? d / 1000.0f : static_cast<float>(d));
				menu::Item it{f.name, k == g_job.dest ? "目前" : dist, help};
				it.enabled = !f.prison || g_job.felony;
				it.action = [k] {
					g_job.dest = k;
					g_job.refused = false;
					Route();
					records::Note(std::string("載送目的地改為") + kFacilities[k].name);
					ml::Notify("改送{}", kFacilities[k].name);
				};
				m.items.push_back(it);
			}
			return m;
		};
		return i;
	}

	void Forget()
	{
		g_job = {};
		g_byBackup.clear();
		g_booked.clear();
		g_escaped = 0;
	}

	void Tick()
	{
		if (!officer::IsPlayer())
		{
			if (g_job.stage != Stage::None)
				Reset();
			return;
		}
		if (g_job.stage == Stage::None)
			return;
		const uint64_t now = ml::TickMs();
		const suspect::Status st = suspect::StatusOf(g_job.id);
		if (st.gone || st.dead)
		{
			Reset();
			return;
		}
		ai::Profile profile;
		suspect::ProfileOf(g_job.ped, profile);
		switch (g_job.stage)
		{
		case Stage::Transport:
		{
			if (!st.custody)
			{
				Reset(); // out of the car some other way (escaped: suspect.cpp)
				return;
			}
			if (g_job.backup)
			{
				if (units::Delivered(g_job.car))
				{
					// The backup booked them: what they carried comes out at the desk.
					if (search::Book(g_job.ped))
						records::Conduct(-10, "交付載送前未搜身（收押時才發現武器）");
					Record("（支援載送）");
					suspect::Delete(g_job.id);
					units::TransportDone(g_job.car);
					ml::Notify("支援：嫌犯已送到{}，收押完成", kFacilities[g_job.dest].name);
					Reset();
				}
				return;
			}
			const ::Ped me = PLAYER::PLAYER_PED_ID();
			const bool driving = PED::IS_PED_IN_VEHICLE(me, g_job.car, false);
			if (driving)
			{
				Driving(now);
				if (now >= g_job.talkAt)
				{
					Talk(profile);
					g_job.talkAt = now + 25000 + static_cast<uint64_t>(Random() * 25000.0f);
				}
			}
			else
				Chance(now, profile);
			if (g_job.stage != Stage::Transport)
				return;
			// There: the car stopped by the door and the officer out.
			const Facility& f = kFacilities[g_job.dest];
			const float d = Distance(ENTITY::GET_ENTITY_COORDS(g_job.car, true), V(f.park));
			if (f.prison && !g_job.felony && d < 60.0f && !g_job.refused)
			{
				g_job.refused = true;
				ml::Notify("監獄只收重罪嫌犯：F9「載送」改送警局");
			}
			if (d < 25.0f && !driving && !(f.prison && !g_job.felony))
				Arrive();
			break;
		}
		case Stage::Arrived:
			TickBooking(now);
			break;
		case Stage::Leaving:
			if (now >= g_job.leaveAt)
				Finish();
			break;
		default:
			break;
		}
	}
}
