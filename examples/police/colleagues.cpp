// The officer's colleagues (police life part 3, the user's addition 2026-10-22, research/phase0.md §56): people of the
// department with a name, a rank, a shift and a personality, and how they get on with the officer (rapport, -100..100).
// - At the station during their shift some of them stand about (the lobby and the briefing room at Mission Row, by the
//   door at the stations without an interior): E next to one to talk.
// - Backup comes from the colleagues on duty (units.cpp), named on the radio, and rapport counts: liked, they come
//   sooner; disliked, some say no and someone else comes, later.
// - The radio (F9): the colleagues on duty, to ask one of them for backup or how things are.
// - The roll call's sergeant is the sergeant of the officer's shift.
// Rapport: a greeting a day, backup that came and was thanked (the scene cleared), help given or refused when they ask
// for it (later).
//
// Kept in the save: ml::save "officer_colleagues_<department>".
#include "police.hpp"

#include <algorithm>
#include <cmath>
#include <format>
#include <string>
#include <vector>

namespace police::colleagues
{
	namespace
	{
		const char* const kMaleFirst[] = {"Marcus", "Derek", "Luis", "Kevin", "Brandon", "Carlos", "Jamal", "Travis", "Andre", "Daniel", "Ryan", "Jose",
		    "Victor", "Sean", "Eddie", "Frank", "Hector", "Paul", "Tony", "Will"};
		const char* const kFemaleFirst[] = {"Emily", "Jessica", "Maria", "Ashley", "Sarah", "Nicole", "Rosa", "Megan", "Linda", "Tanya", "Grace", "Elena"};
		const char* const kLast[] = {"Johnson", "Ramirez", "Walker", "Nguyen", "Brooks", "Morales", "Carter", "Hughes", "Reyes", "Bennett", "Kim",
		    "Lopez", "Miller", "Davis", "Garcia", "Wilson", "Anderson", "Thomas", "Moore", "Jackson", "Ortiz", "Park", "Sullivan", "Chen"};

		// Personalities: what they say when greeted (rapport low / middling / high; {} = the officer's name) and how they
		// answer on the radio.
		struct Trait
		{
			const char* name;
			const char* hello[3];
			const char* radio;
		};
		const Trait kTraits[] = {
		    {"熱心", {"嗯，你好。", "嘿，{}！今天還好嗎？有需要就叫我。", "{}！來得正好，咖啡我幫你倒了一杯。"}, "這邊一切正常，你那邊需要幫忙就說。"},
		    {"嚴肅", {"報告寫好了嗎？", "早。照程序來，別給大家添麻煩。", "你最近表現不錯，繼續保持。"}, "巡邏中，狀況正常，完畢。"},
		    {"愛開玩笑", {"喔，是你啊。", "又是你，今天別又把巡邏車開進水溝。", "{}！聽說你追車追到嫌犯都想自首了，哈！"}, "無聊到快睡著了，誰來搶個銀行吧……開玩笑的。"},
		    {"老鳥", {"菜鳥，別擋路。", "幹這行久了就知道，活著回家最重要。", "這區的巷子我閉著眼睛都會走，有事問我。"}, "老樣子，平安無事。"},
		    {"菜鳥", {"呃……你好。", "前輩好！今天有什麼要注意的嗎？", "{}前輩！上次你教我的我都記住了！"}, "報告，目前巡邏中，沒有狀況！"},
		    {"冷淡", {"……", "嗯。", "喔，是你。今天還行。"}, "沒事。"},
		    {"衝動", {"看什麼看？", "今天最好來點刺激的，巡邏快無聊死了。", "有追車記得叫我，我馬上到！"}, "安靜得要命，有追車記得叫我。"},
		};

		struct Colleague
		{
			int id = 0;
			std::string name;
			bool female = false;
			int rank = 0;
			int kind = 0; // the shift (shift.cpp: 0 day, 1 evening, 2 night)
			int unit = 0; // the callsign's number
			int trait = 0;
			int rapport = 0;
			int seed = 0; // the face
			int years = 1;
			int64_t greeted = -1; // the game day of the last greeting
		};
		std::vector<Colleague> g;
		int g_department = -1;
		bool g_loaded = false;
		std::vector<int> g_busy; // out as backup or at an incident

		// The departments' police peds (look::Department()): male, female (nullptr: none).
		const char* const kModels[4][2] = {
		    {"s_m_y_cop_01", "s_f_y_cop_01"}, {"s_m_y_sheriff_01", "s_f_y_sheriff_01"}, {"s_m_y_hwaycop_01", nullptr}, {"s_m_y_swat_01", nullptr}};
		constexpr const char* kCallLetter[4] = {"L", "S", "H", "N"};

		// Where they stand about at the stations (a place on the navmesh near each is used).
		const std::vector<Place> kSpots[4] = {
		    {{440.3f, -982.0f, 30.69f, 90.0f}, {434.5f, -984.5f, 30.69f, 0.0f}, {443.0f, -986.0f, 30.69f, 180.0f}, {441.5f, -991.0f, 30.69f, 120.0f},
		        {436.8f, -990.8f, 30.69f, 240.0f}},
		    {{1851.5f, 3683.0f, 34.3f, 200.0f}, {1849.0f, 3678.5f, 33.9f, 300.0f}, {1856.5f, 3676.0f, 33.6f, 30.0f}},
		    {{829.0f, -1283.0f, 28.2f, 90.0f}, {825.0f, -1279.0f, 26.3f, 180.0f}, {831.0f, -1276.0f, 26.3f, 0.0f}},
		    {{2480.0f, -386.5f, 94.4f, 270.0f}, {2485.0f, -381.0f, 93.7f, 180.0f}, {2488.0f, -388.0f, 93.7f, 0.0f}},
		};
		const char* const kScenarios[] = {"WORLD_HUMAN_COP_IDLES", "WORLD_HUMAN_AA_COFFEE", "WORLD_HUMAN_STAND_MOBILE", "WORLD_HUMAN_CLIPBOARD"};

		int Rand(int lo, int hi)
		{
			return MISC::GET_RANDOM_INT_IN_RANGE(lo, hi + 1);
		}

		int Department()
		{
			return std::clamp(look::Department(), 0, 3);
		}

		std::string Key()
		{
			return std::format("officer_colleagues_{}", Department());
		}

		void Store()
		{
			ml::Json list = ml::Json::Array();
			for (const Colleague& c : g)
				list.Push(ml::Json{{"id", c.id}, {"name", c.name}, {"female", c.female}, {"rank", c.rank}, {"kind", c.kind}, {"unit", c.unit},
				    {"trait", c.trait}, {"rapport", c.rapport}, {"seed", c.seed}, {"years", c.years}, {"greeted", c.greeted}});
			ml::save::Set(Key().c_str(), list);
		}

		// The department's people, made the first time (12: officers to a lieutenant, four to each shift).
		void Make()
		{
			g.clear();
			const int dept = Department();
			const bool women = kModels[dept][1] != nullptr;
			constexpr int kRanks[] = {0, 0, 1, 1, 1, 2, 2, 2, 3, 3, 4, 5};
			std::vector<int> units;
			std::vector<std::string> names;
			for (int i = 0; i < 12; ++i)
			{
				Colleague c;
				c.id = i + 1;
				c.female = women && Rand(0, 99) < 35;
				do
				{
					const std::string first = c.female ? kFemaleFirst[Rand(0, static_cast<int>(std::size(kFemaleFirst)) - 1)]
					                                   : kMaleFirst[Rand(0, static_cast<int>(std::size(kMaleFirst)) - 1)];
					c.name = first + " " + kLast[Rand(0, static_cast<int>(std::size(kLast)) - 1)];
				} while (std::find(names.begin(), names.end(), c.name) != names.end());
				names.push_back(c.name);
				c.rank = kRanks[i];
				c.kind = i % 3;
				do
					c.unit = Rand(10, 89);
				while (std::find(units.begin(), units.end(), c.unit) != units.end());
				units.push_back(c.unit);
				c.trait = c.rank == 0 && Rand(0, 1) ? 4 : Rand(0, static_cast<int>(std::size(kTraits)) - 1);
				if (c.trait == 4 && c.rank > 1)
					c.trait = 3; // a sergeant is no rookie
				c.seed = Rand(0, 9999);
				c.years = 1 + c.rank * 3 + Rand(0, 3);
				g.push_back(c);
			}
			ml::Log("colleagues: made the {} people of department {}", g.size(), dept);
			Store();
		}

		void Load()
		{
			if (g_loaded && g_department == Department())
				return;
			g_loaded = true;
			g_department = Department();
			g.clear();
			g_busy.clear();
			for (const ml::Json& j : ml::save::Get(Key().c_str()).Items())
			{
				Colleague c;
				c.id = j["id"].Int();
				c.name = j["name"].Str();
				c.female = j["female"].Bool();
				c.rank = j["rank"].Int();
				c.kind = std::clamp(j["kind"].Int(), 0, 2);
				c.unit = j["unit"].Int();
				c.trait = std::clamp(j["trait"].Int(), 0, static_cast<int>(std::size(kTraits)) - 1);
				c.rapport = std::clamp(j["rapport"].Int(), -100, 100);
				c.seed = j["seed"].Int();
				c.years = j["years"].Int(1);
				c.greeted = j["greeted"].Int64(-1);
				g.push_back(c);
			}
			if (g.empty())
				Make();
		}

		Colleague* Find(int id)
		{
			Load();
			for (Colleague& c : g)
				if (c.id == id)
					return &c;
			return nullptr;
		}

		bool Busy(int id)
		{
			return std::find(g_busy.begin(), g_busy.end(), id) != g_busy.end();
		}

		// On duty now: their shift's hours on one of their work days.
		bool OnDuty(const Colleague& c)
		{
			const int start = c.kind == 0 ? 7 : c.kind == 1 ? 15 : 23;
			const int hour = CLOCK::GET_CLOCK_HOURS();
			const int into = (hour - start + 24) % 24; // hours into the shift that began last at `start`
			if (into >= 8)
				return false;
			// The day the shift began (a night shift after midnight began yesterday).
			const int64_t day = shift::Day() - (hour < start ? 1 : 0);
			return ((day + c.id * 2) % 7 + 7) % 7 < 5;
		}

		std::string RapportName(int r)
		{
			return r < -40 ? "很差" : r < -10 ? "冷淡" : r < 20 ? "普通" : r < 50 ? "不錯" : "很好";
		}

		std::string Title(const Colleague& c)
		{
			return career::Title(c.rank);
		}

		std::string CallsignOf(const Colleague& c)
		{
			return std::format("{}-{}-{:02}", 1 + c.id % 5, kCallLetter[Department()], c.unit);
		}

		std::string Me()
		{
			const std::string name = look::Name();
			if (name.empty())
				return "夥伴";
			const size_t space = name.rfind(' ');
			return space == std::string::npos ? name : name.substr(space + 1);
		}

		std::string Fill(const char* line)
		{
			std::string s = line;
			if (const size_t at = s.find("{}"); at != std::string::npos)
				s.replace(at, 2, Me());
			return s;
		}

		// A line said by someone (a subtitle at the bottom of the screen).
		void Say(const Colleague& c, const std::string& line, ::Ped ped)
		{
			HUD::BEGIN_TEXT_COMMAND_PRINT("STRING");
			HUD::ADD_TEXT_COMPONENT_SUBSTRING_PLAYER_NAME((c.name + "：" + line).c_str());
			HUD::END_TEXT_COMMAND_PRINT(5000, true);
			if (ped && ENTITY::DOES_ENTITY_EXIST(ped))
				AUDIO::PLAY_PED_AMBIENT_SPEECH_NATIVE(ped, "GENERIC_HI", "SPEECH_PARAMS_FORCE", 0);
		}

		// ---- at the station -----------------------------------------------------------------------------------------
		struct Present
		{
			int id = 0;
			::Ped ped = 0;
			size_t spot = 0;
		};
		std::vector<Present> g_present;
		uint64_t g_nextPresence = 0;

		bool Exists(::Ped p)
		{
			return p && ENTITY::DOES_ENTITY_EXIST(p);
		}

		void Remove(Present& p)
		{
			if (Exists(p.ped))
			{
				ENTITY::SET_ENTITY_AS_MISSION_ENTITY(p.ped, true, true);
				PED::DELETE_PED(&p.ped);
			}
			p.ped = 0;
		}

		// At the station this game hour: some of those on duty (by the hour, for a while at a time).
		bool AtStationNow(const Colleague& c)
		{
			if (!OnDuty(c) || Busy(c.id))
				return false;
			const int hour = CLOCK::GET_CLOCK_HOURS();
			const int start = c.kind == 0 ? 7 : c.kind == 1 ? 15 : 23;
			if ((hour - start + 24) % 24 == 0 || (hour - start + 24) % 24 == 7)
				return true; // the shift's first and last hour
			return (c.seed + hour * 37 + c.id * 11) % 100 < 30;
		}

		::Ped MakePed(const Colleague& c, const Vector3& at, float heading)
		{
			const int dept = Department();
			const char* modelName = c.female && kModels[dept][1] ? kModels[dept][1] : kModels[dept][0];
			const Hash model = MISC::GET_HASH_KEY(modelName);
			const auto loaded = ml::LoadModel(model);
			if (!loaded)
				return 0;
			const ::Ped p = PED::CREATE_PED(6, model, at.x, at.y, at.z, heading, false, false);
			if (!p)
				return 0;
			ENTITY::SET_ENTITY_AS_MISSION_ENTITY(p, true, true);
			Dress(p, c.id);
			PED::SET_PED_RELATIONSHIP_GROUP_HASH(p, MISC::GET_HASH_KEY("COP"));
			PED::SET_PED_CAN_BE_TARGETTED_BY_PLAYER(p, PLAYER::PLAYER_ID(), false);
			return p;
		}

		void Presence()
		{
			const bool player = officer::IsPlayer();
			const Vector3 me = ENTITY::GET_ENTITY_COORDS(PLAYER::PLAYER_PED_ID(), true);
			const Place station = duty::StationPlace();
			const bool nearby = player && ai::Distance(me, {station.x, station.y, station.z}) < 120.0f;
			// Those who should not be here (any more) go when out of sight.
			for (Present& p : g_present)
			{
				const Colleague* c = Find(p.id);
				const bool stay = nearby && c && AtStationNow(*c);
				if (!stay && (!nearby || !Exists(p.ped) || !ENTITY::IS_ENTITY_ON_SCREEN(p.ped)))
					Remove(p);
			}
			std::erase_if(g_present, [](const Present& p) { return !p.ped; });
			if (!nearby)
				return;
			const auto& spots = kSpots[Department()];
			for (const Colleague& c : g)
			{
				if (g_present.size() >= spots.size())
					break;
				if (!AtStationNow(c) || std::any_of(g_present.begin(), g_present.end(), [&](const Present& p) { return p.id == c.id; }))
					continue;
				size_t spot = 0;
				while (spot < spots.size() && std::any_of(g_present.begin(), g_present.end(), [&](const Present& p) { return p.spot == spot; }))
					++spot;
				if (spot >= spots.size())
					break;
				const Place& s = spots[spot];
				// The spot itself, or a place on the navmesh right by it (the safe-place search may jump elsewhere).
				Vector3 at{s.x, s.y, s.z};
				if (Vector3 snapped{}; ai::OnFoot(s.x, s.y, s.z, snapped) && ai::Distance(snapped, at) < 1.5f)
					at = snapped;
				if (CAMERA::IS_SPHERE_VISIBLE(at.x, at.y, at.z, 1.0f))
					continue; // (not where the officer would see them appear)
				const ::Ped ped = MakePed(c, at, s.heading);
				if (!ped)
					continue;
				TASK::TASK_START_SCENARIO_IN_PLACE(ped, kScenarios[(c.seed + spot) % std::size(kScenarios)], 0, true);
				g_present.push_back({c.id, ped, spot});
				ml::Log("colleagues: {} at the station (spot {}, {:.1f} {:.1f} {:.1f})", c.name, spot, at.x, at.y, at.z);
			}
		}

		// Their name over their head, close by.
		void NameTags()
		{
			const Vector3 me = ENTITY::GET_ENTITY_COORDS(PLAYER::PLAYER_PED_ID(), true);
			for (const Present& p : g_present)
			{
				if (!Exists(p.ped) || !ENTITY::IS_ENTITY_ON_SCREEN(p.ped))
					continue;
				const Vector3 at = ENTITY::GET_ENTITY_COORDS(p.ped, true);
				if (ai::Distance(at, me) > 8.0f)
					continue;
				const Colleague* c = Find(p.id);
				if (!c)
					continue;
				GRAPHICS::SET_DRAW_ORIGIN(at.x, at.y, at.z + 1.05f, false);
				HUD::SET_TEXT_SCALE(0.0f, 0.32f);
				HUD::SET_TEXT_FONT(0);
				HUD::SET_TEXT_CENTRE(true);
				HUD::SET_TEXT_OUTLINE();
				HUD::SET_TEXT_COLOUR(255, 255, 255, 230);
				HUD::BEGIN_TEXT_COMMAND_DISPLAY_TEXT("STRING");
				HUD::ADD_TEXT_COMPONENT_SUBSTRING_PLAYER_NAME((Title(*c) + " " + c->name).c_str());
				HUD::END_TEXT_COMMAND_DISPLAY_TEXT(0.0f, 0.0f, 0);
				GRAPHICS::CLEAR_DRAW_ORIGIN();
			}
		}

		// Standing about at the station now.
		bool Here(int id)
		{
			return std::any_of(g_present.begin(), g_present.end(), [id](const Present& p) { return p.id == id && Exists(p.ped); });
		}

		void Greet(int id, ::Ped ped)
		{
			Colleague* c = Find(id);
			if (!c)
				return;
			const int level = c->rapport < -20 ? 0 : c->rapport > 40 ? 2 : 1;
			Say(*c, Fill(kTraits[c->trait].hello[level]), ped);
			const int64_t today = shift::Day();
			if (c->greeted != today)
			{
				c->greeted = today;
				if (c->rapport < 30)
					Rapport(id, 1, "打招呼");
				else
					Store();
			}
		}

		void TalkMenu(int id, ::Ped ped)
		{
			const Colleague* c = Find(id);
			if (!c)
				return;
			TASK::CLEAR_PED_TASKS(ped);
			TASK::TASK_TURN_PED_TO_FACE_ENTITY(ped, PLAYER::PLAYER_PED_ID(), -1);
			menu::List m{Title(*c) + " " + c->name, {}};
			m.items.push_back({"關係", RapportName(c->rapport), std::format("{}，{}班，年資 {} 年，呼號 {}", kTraits[c->trait].name,
			                                                         c->kind == 0 ? "日" : c->kind == 1 ? "小夜" : "大夜", c->years, CallsignOf(*c))});
			menu::Item hello{"打招呼", "", ""};
			hello.action = [id, ped] { Greet(id, ped); };
			m.items.push_back(hello);
			menu::Item beat{"問問勤務區的狀況", "", "今天勤務區和協尋要注意什麼"};
			beat.action = [id, ped] {
				if (const Colleague* c = Find(id))
					Say(*c, std::format("你今天在{}吧？{}。協尋那台{}多注意一下。", shift::BeatName(), shift::FocusText(), shift::BoloCar()), ped);
			};
			m.items.push_back(beat);
			const int seed = c->seed;
			m.leave = [ped, seed] {
				if (Exists(ped))
					TASK::TASK_START_SCENARIO_IN_PLACE(ped, kScenarios[static_cast<size_t>(seed) % std::size(kScenarios)], 0, true);
			};
			menu::Open(m);
		}

		void Talk()
		{
			if (menu::IsOpen() || !officer::IsPlayer())
				return;
			const ::Ped pl = PLAYER::PLAYER_PED_ID();
			if (PED::IS_PED_IN_ANY_VEHICLE(pl, false))
				return;
			const Vector3 me = ENTITY::GET_ENTITY_COORDS(pl, true);
			if (duty::NearLocker(me))
				return;
			for (const Present& p : g_present)
			{
				if (!Exists(p.ped) || ai::Distance(ENTITY::GET_ENTITY_COORDS(p.ped, true), me) > 2.0f)
					continue;
				const Colleague* c = Find(p.id);
				if (!c)
					continue;
				HUD::BEGIN_TEXT_COMMAND_DISPLAY_HELP("STRING");
				HUD::ADD_TEXT_COMPONENT_SUBSTRING_PLAYER_NAME(("按 ~INPUT_CONTEXT~ 跟" + c->name + "說話").c_str());
				HUD::END_TEXT_COMMAND_DISPLAY_HELP(0, false, false, -1);
				if (PAD::IS_CONTROL_JUST_PRESSED(0, 51))
					TalkMenu(p.id, p.ped);
				return;
			}
		}
	}

	void Dress(::Ped ped, int id)
	{
		const Colleague* c = Find(id);
		if (!c || !Exists(ped))
			return;
		// The same face every time (their seed), the rest the model's own.
		PED::SET_PED_DEFAULT_COMPONENT_VARIATION(ped);
		for (const int component : {0, 2})
			if (const int n = PED::GET_NUMBER_OF_PED_DRAWABLE_VARIATIONS(ped, component); n > 0)
			{
				const int drawable = c->seed % n;
				const int textures = std::max(1, PED::GET_NUMBER_OF_PED_TEXTURE_VARIATIONS(ped, component, drawable));
				PED::SET_PED_COMPONENT_VARIATION(ped, component, drawable, (c->seed / 7) % textures, 0);
			}
	}

	Hash Model(int id)
	{
		const Colleague* c = Find(id);
		const int dept = Department();
		return MISC::GET_HASH_KEY(c && c->female && kModels[dept][1] ? kModels[dept][1] : kModels[dept][0]);
	}

	std::string Name(int id)
	{
		const Colleague* c = Find(id);
		return c ? c->name : "";
	}

	std::string Callsign(int id)
	{
		const Colleague* c = Find(id);
		return c ? CallsignOf(*c) : "";
	}

	std::string Sergeant(int kind)
	{
		Load();
		for (const Colleague& c : g)
			if (c.rank >= 3 && c.rank <= 4 && c.kind == kind)
				return Title(c) + " " + c.name;
		return "";
	}

	void SetBusy(int id, bool busy)
	{
		std::erase(g_busy, id);
		if (busy)
			g_busy.push_back(id);
	}

	void Rapport(int id, int delta, const char* why)
	{
		Colleague* c = Find(id);
		if (!c || !delta)
			return;
		c->rapport = std::clamp(c->rapport + delta, -100, 100);
		Store();
		ml::Log("colleagues: {} rapport {:+} ({}) -> {}", c->name, delta, why, c->rapport);
	}

	int PickBackup(int wanted, float& minDistance, float& maxDistance)
	{
		Load();
		minDistance = 120.0f;
		maxDistance = 300.0f;
		std::vector<const Colleague*> free;
		for (const Colleague& c : g)
			if (OnDuty(c) && !Busy(c.id) && !Here(c.id) && (wanted < 0 || c.id == wanted))
				free.push_back(&c);
		// Those who like the officer first.
		std::sort(free.begin(), free.end(), [](const Colleague* a, const Colleague* b) { return a->rapport + (a->seed % 20) > b->rapport + (b->seed % 20); });
		bool declined = false;
		for (const Colleague* c : free)
		{
			if (c->rapport < -30 && ai::Random() < 0.5f)
			{
				ml::Notify("（無線電）{} {}：我這邊走不開，找別人吧。", CallsignOf(*c), c->name);
				ml::Log("colleagues: {} would not come (rapport {})", c->name, c->rapport);
				declined = true;
				continue;
			}
			if (c->rapport > 50)
				minDistance = 100.0f, maxDistance = 180.0f;
			else if (declined || c->rapport < -10)
				minDistance = 250.0f, maxDistance = 400.0f;
			return c->id;
		}
		if (declined)
			minDistance = 300.0f, maxDistance = 450.0f;
		return -1;
	}

	menu::Item RadioItem()
	{
		menu::Item radio{"無線電", "", "值勤中的同事：請求支援、問狀況"};
		radio.sub = [] {
			Load();
			menu::List m{"無線電", {}};
			for (const Colleague& c : g)
			{
				if (!OnDuty(c))
					continue;
				const int id = c.id;
				menu::Item i{CallsignOf(c) + " " + c.name, Busy(id) ? "忙碌" : Here(id) ? "在警局" : RapportName(c.rapport), Title(c) + "，" + kTraits[c.trait].name};
				i.sub = [id] {
					const Colleague* c = Find(id);
					menu::List s{c ? CallsignOf(*c) + " " + c->name : "", {}};
					menu::Item backup{"請他支援", "", Here(id) ? "他在警局裡，直接找他說話" : "請這位同事到你這裡支援"};
					backup.enabled = !Busy(id) && !Here(id);
					backup.action = [id] { units::CallBackup(id); };
					s.items.push_back(backup);
					menu::Item status{"問狀況", "", ""};
					status.action = [id] {
						if (const Colleague* c = Find(id))
							ml::Notify("（無線電）{} {}：{}", CallsignOf(*c), c->name, Busy(id) ? "在支援現場，晚點再說。" : Here(id) ? "我在警局，有事過來說。" : kTraits[c->trait].radio);
					};
					s.items.push_back(status);
					return s;
				};
				m.items.push_back(i);
			}
			if (m.items.empty())
				m.items.push_back({"沒有同事在線上", "", ""});
			return m;
		};
		return radio;
	}

	ml::Json Json()
	{
		Load();
		ml::Json list = ml::Json::Array();
		for (const Colleague& c : g)
			list.Push(ml::Json{{"id", c.id}, {"name", c.name}, {"title", Title(c)}, {"female", c.female}, {"trait", kTraits[c.trait].name},
			    {"shift", c.kind == 0 ? "日班" : c.kind == 1 ? "小夜班" : "大夜班"}, {"callsign", CallsignOf(c)}, {"years", c.years},
			    {"rapport", c.rapport}, {"rapportName", RapportName(c.rapport)}, {"onDuty", OnDuty(c)}, {"busy", Busy(c.id)}});
		return list;
	}

	void Loading()
	{
		g_loaded = false;
		g_present.clear();
		g_busy.clear();
	}

	void Tick()
	{
		if (!officer::IsPlayer())
		{
			for (Present& p : g_present)
				Remove(p);
			g_present.clear();
			return;
		}
		Load();
		if (ml::TickMs() >= g_nextPresence)
		{
			g_nextPresence = ml::TickMs() + 1000;
			Presence();
		}
		NameTags();
		Talk();
	}
}
