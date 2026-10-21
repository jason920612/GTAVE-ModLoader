// Searches (user's design): a person, or a vehicle area by area (driver's seat, front passenger seat, glovebox, back
// seats, boot), each taking a few seconds: whoever searches walks to the door, opens it, looks through, closes it.
// The officer can do it or hand it to a backup officer standing by, who reports over the radio when done. What there
// is to find follows who the suspect is (suspect::ProfileOf): their weapon, a controlled substance of its grade, an
// open bottle when they are drunk, what was in a stolen car's boot, and the ordinary things everyone has.
#include "police.hpp"

#include <algorithm>
#include <cmath>
#include <map>
#include <string>
#include <utility>
#include <vector>

namespace police::search
{
	namespace
	{
		using ai::Distance;
		using ai::Random;

		struct Found
		{
			std::string what;
			bool evidence = false;
			std::string charge; // what it supports
		};

		struct Contents
		{
			std::vector<Found> zones[kZones];
			bool searched[kZones] = {};
		};
		std::map<Vehicle, Contents> g_cars;
		std::map<::Ped, std::vector<Found>> g_people;
		std::map<::Ped, bool> g_frisked;

		const char* const kZoneNames[kZones] = {"駕駛座", "副駕駛座", "置物箱", "後座", "後車廂"};
		const char* const kGrades[] = {"", "第一級", "第二級", "第三級", "第四級"};

		Found Weapon(int armed)
		{
			if (armed >= 3)
				return {"衝鋒槍一把（已上膛）", true, "非法持有武器"};
			if (armed == 2)
				return {"手槍一把", true, "非法持有武器"};
			return {"折疊刀一把", true, "持有危險器械"};
		}
		Found Substance(int grade)
		{
			grade = std::clamp(grade, 1, 4);
			return {std::string(kGrades[grade]) + "管制藥物（小包裝數包）", true, std::string("持有") + kGrades[grade] + "管制藥物"};
		}
		Found Clutter(int zone)
		{
			static const char* const front[] = {"收據幾張", "太陽眼鏡", "手機充電線", "速食包裝", "停車單"};
			static const char* const glove[] = {"行照", "保險卡", "面紙", "車主手冊"};
			static const char* const back[] = {"運動包（衣物）", "雨傘", "購物袋", "兒童座椅"};
			static const char* const boot[] = {"備胎和工具", "紙箱", "運動用品", "購物袋"};
			const auto pick = [](const auto& list) { return std::string(list[MISC::GET_RANDOM_INT_IN_RANGE(0, static_cast<int>(std::size(list)))]); };
			switch (zone)
			{
			case 2: return {pick(glove)};
			case 3: return {pick(back)};
			case 4: return {pick(boot)};
			default: return {pick(front)};
			}
		}

		Contents& CarContents(Vehicle car)
		{
			auto it = g_cars.find(car);
			if (it != g_cars.end())
				return it->second;
			Contents c;
			ai::Profile p;
			bool suspect = false;
			if (const ::Ped owner = suspect::OwnerOf(car))
				suspect = suspect::ProfileOf(owner, p);
			for (int z = 0; z < kZones; ++z)
				if (Random() < 0.6f)
					c.zones[z].push_back(Clutter(z));
			if (suspect)
			{
				if (p.armed >= 1 && Random() < 0.6f)
					c.zones[Random() < 0.5f ? 0 : 2].push_back(Weapon(p.armed));
				if (p.substanceGrade > 0 && Random() < 0.7f)
					c.zones[Random() < 0.5f ? 2 : 3].push_back(Substance(p.substanceGrade));
				else if (p.guilt > 0.75f && Random() < 0.3f)
					c.zones[2].push_back(Substance(MISC::GET_RANDOM_INT_IN_RANGE(1, 5)));
				if (p.impairment == ai::Impairment::Drunk)
					c.zones[1].push_back({"開封的酒瓶", true, "車內有開封酒類"});
				if (records::VehicleOf(car).stolen && Random() < 0.5f)
					c.zones[4].push_back({"疑似贓物（筆電、首飾）", true, "收受贓物"});
			}
			return g_cars.emplace(car, c).first->second;
		}

		std::vector<Found>& PersonContents(::Ped ped)
		{
			auto it = g_people.find(ped);
			if (it != g_people.end())
				return it->second;
			std::vector<Found> f;
			ai::Profile p;
			if (suspect::ProfileOf(ped, p))
			{
				if (p.armed >= 1)
					f.push_back(Weapon(p.armed));
				if (p.substanceGrade > 0)
					f.push_back(Substance(p.substanceGrade));
			}
			f.push_back({"皮夾（證件、現金 $" + std::to_string(MISC::GET_RANDOM_INT_IN_RANGE(5, 400)) + "）"});
			if (Random() < 0.6f)
				f.push_back({"手機"});
			if (Random() < 0.3f)
				f.push_back({"鑰匙一串"});
			return g_people.emplace(ped, f).first->second;
		}

		std::string Describe(const std::vector<Found>& items)
		{
			std::string evidence, other;
			for (const Found& f : items)
				(f.evidence ? evidence : other) += (f.evidence ? evidence : other).empty() ? f.what : "、" + f.what;
			if (!evidence.empty())
				return "發現：" + evidence + (other.empty() ? "" : "；另有 " + other);
			return other.empty() ? "沒有東西" : "只有 " + other;
		}

		void Record(const std::vector<Found>& items, const std::string& where)
		{
			for (const Found& f : items)
				if (f.evidence)
				{
					records::Evidence(f.what + "（" + where + "）");
					if (!f.charge.empty())
						records::Charge(f.charge);
				}
			records::Note("搜查" + where + "：" + Describe(items));
		}

		// ---- the work: walk there, open up, look, close
		struct Job
		{
			Vector3 at{};
			::Ped who = 0;
			bool officer = false;   // the player (else a backup officer)
			bool leaving = false;   // getting out of their car first
			Vehicle car = 0;
			::Ped target = 0;       // a person to frisk
			std::vector<int> zones; // areas still to do (vehicle)
			int stage = 0;          // 0 walk, 1 search
			uint64_t until = 0;
			uint64_t started = 0;
		};
		std::vector<Job> g_jobs;

		int Door(int zone)
		{
			switch (zone)
			{
			case 0: return 0;
			case 1: case 2: return 1;
			case 3: return 2;
			default: return 5;
			}
		}

		// Where to stand for an area: by its door, a step out.
		Vector3 Spot(Vehicle car, int zone)
		{
			Vector3 mn{}, mx{};
			MISC::GET_MODEL_DIMENSIONS(ENTITY::GET_ENTITY_MODEL(car), &mn, &mx);
			switch (zone)
			{
			case 0: return ENTITY::GET_OFFSET_FROM_ENTITY_IN_WORLD_COORDS(car, mn.x - 0.6f, 0.4f, 0.0f);
			case 1: case 2: return ENTITY::GET_OFFSET_FROM_ENTITY_IN_WORLD_COORDS(car, mx.x + 0.6f, 0.4f, 0.0f);
			case 3: return ENTITY::GET_OFFSET_FROM_ENTITY_IN_WORLD_COORDS(car, mn.x - 0.6f, -0.7f, 0.0f);
			default: return ENTITY::GET_OFFSET_FROM_ENTITY_IN_WORLD_COORDS(car, 0.0f, mn.y - 0.8f, 0.0f);
			}
		}

		void Walk(Job& j)
		{
			j.at = j.car ? Spot(j.car, j.zones.front()) : ENTITY::GET_OFFSET_FROM_ENTITY_IN_WORLD_COORDS(j.target, 0.0f, -0.9f, 0.0f);
			TASK::TASK_FOLLOW_NAV_MESH_TO_COORD(j.who, j.at.x, j.at.y, j.at.z, 1.0f, 15000, 0.4f, 0, 0.0f);
			j.stage = 0;
			j.started = ml::TickMs();
		}

		void Tell(const Job& j, const std::string& text)
		{
			ml::Log("search: {} {}", j.officer ? "officer" : "backup", text);
			if (j.officer)
				ml::Notify("{}", text);
			else
				ml::Notify("支援：{}", text);
		}

		// True when the job is over.
		bool Step(Job& j)
		{
			const uint64_t now = ml::TickMs();
			if (!ENTITY::DOES_ENTITY_EXIST(j.who) || ENTITY::IS_ENTITY_DEAD(j.who, false) || (j.car && !ENTITY::DOES_ENTITY_EXIST(j.car)) ||
			    (j.target && !ENTITY::DOES_ENTITY_EXIST(j.target)))
				return true;
			// The officer gives up on it by walking off.
			if (j.officer && (std::abs(PAD::GET_CONTROL_NORMAL(0, 30)) > 0.5f || std::abs(PAD::GET_CONTROL_NORMAL(0, 31)) > 0.5f) && now - j.started > 600)
			{
				TASK::CLEAR_PED_TASKS(j.who);
				ml::Notify("搜查中斷");
				return true;
			}
			if (j.stage == 0)
			{
				// A backup officer still in their car gets out first.
				if (PED::IS_PED_IN_ANY_VEHICLE(j.who, false))
				{
					if (!j.leaving)
					{
						j.leaving = true;
						TASK::TASK_LEAVE_VEHICLE(j.who, PED::GET_VEHICLE_PED_IS_IN(j.who, false), 0);
					}
					j.started = now; // (the walk's time starts once out)
					return false;
				}
				if (std::exchange(j.leaving, false))
				{
					Walk(j);
					return false;
				}
				const Vector3 me = ENTITY::GET_ENTITY_COORDS(j.who, true);
				if (Distance(me, j.at) < 1.0f || now - j.started > 15000)
				{
					// Facing it, the door open, looking through.
					const Vector3 face = j.car ? ENTITY::GET_ENTITY_COORDS(j.car, true) : ENTITY::GET_ENTITY_COORDS(j.target, true);
					TASK::TASK_TURN_PED_TO_FACE_COORD(j.who, face.x, face.y, face.z, 800);
					if (j.car)
						VEHICLE::SET_VEHICLE_DOOR_OPEN(j.car, Door(j.zones.front()), false, false);
					TASK::TASK_START_SCENARIO_IN_PLACE(j.who, j.car ? "PROP_HUMAN_BUM_BIN" : "CODE_HUMAN_POLICE_INVESTIGATE", 0, true);
					j.stage = 1;
					j.until = now + (j.car ? 4500 : 3500);
				}
				return false;
			}
			if (now < j.until)
				return false;
			TASK::CLEAR_PED_TASKS(j.who);
			if (!j.car)
			{
				auto& items = PersonContents(j.target);
				g_frisked[j.target] = true;
				Record(items, "嫌犯身上");
				Tell(j, "搜身：" + Describe(items));
				suspect::Disarm(j.target);
				return true;
			}
			const int zone = j.zones.front();
			Contents& c = CarContents(j.car);
			c.searched[zone] = true;
			Record(c.zones[zone], std::string("車內") + kZoneNames[zone]);
			Tell(j, std::string("搜車（") + kZoneNames[zone] + "）：" + Describe(c.zones[zone]));
			VEHICLE::SET_VEHICLE_DOOR_SHUT(j.car, Door(zone), false);
			j.zones.erase(j.zones.begin());
			if (j.zones.empty())
				return true;
			Walk(j);
			return false;
		}

		void Start(::Ped who, bool officer, Vehicle car, ::Ped target, std::vector<int> zones)
		{
			if (std::any_of(g_jobs.begin(), g_jobs.end(), [&](const Job& j) { return j.who == who; }))
			{
				ml::Notify("{}", officer ? "你正在搜查" : "那位支援正在忙");
				return;
			}
			Job j;
			j.who = who;
			j.officer = officer;
			j.car = car;
			j.target = target;
			j.zones = std::move(zones);
			Walk(j);
			g_jobs.push_back(j);
		}

		std::vector<int> Unsearched(Vehicle car)
		{
			const Contents& c = CarContents(car);
			std::vector<int> z;
			const bool bike = VEHICLE::IS_THIS_MODEL_A_BIKE(ENTITY::GET_ENTITY_MODEL(car));
			for (int i = 0; i < kZones; ++i)
				if (!c.searched[i] && !(bike && i != 4 && i != 0) && !std::any_of(g_jobs.begin(), g_jobs.end(), [&](const Job& j) {
					    return j.car == car && std::find(j.zones.begin(), j.zones.end(), i) != j.zones.end();
				    }))
					z.push_back(i);
			return z;
		}
	}

	const char* ZoneName(int zone)
	{
		return zone >= 0 && zone < kZones ? kZoneNames[zone] : "";
	}

	bool Searched(Vehicle car, int zone)
	{
		return CarContents(car).searched[zone];
	}

	bool Frisked(::Ped ped)
	{
		return g_frisked[ped];
	}

	bool Busy()
	{
		return std::any_of(g_jobs.begin(), g_jobs.end(), [](const Job& j) { return j.officer; });
	}

	void Frisk(::Ped target, bool backup)
	{
		if (backup)
		{
			const ::Ped helper = units::Borrow(ENTITY::GET_ENTITY_COORDS(target, true));
			if (!helper)
			{
				ml::Notify("附近沒有可以幫忙的支援");
				return;
			}
			Start(helper, false, 0, target, {});
			ml::Notify("支援：收到，我來搜他");
		}
		else
			Start(PLAYER::PLAYER_PED_ID(), true, 0, target, {});
	}

	bool Book(::Ped target)
	{
		auto& items = PersonContents(target);
		ml::Notify("保管財物：{}", Describe(items));
		if (g_frisked[target])
		{
			records::Note("收押：保管財物（" + Describe(items) + "）");
			return false;
		}
		// Nobody searched them: what they carry comes out at the desk.
		g_frisked[target] = true;
		Record(items, "收押清點時嫌犯身上");
		suspect::Disarm(target);
		ml::Log("search: booking found {}", Describe(items));
		return std::any_of(items.begin(), items.end(), [](const Found& f) { return f.charge == "非法持有武器" || f.charge == "持有危險器械"; });
	}

	void SearchCar(Vehicle car, int zone, bool backup)
	{
		std::vector<int> zones = zone >= 0 ? std::vector<int>{zone} : Unsearched(car);
		if (zones.empty())
		{
			ml::Notify("這台車已經搜完了");
			return;
		}
		if (backup)
		{
			const ::Ped helper = units::Borrow(ENTITY::GET_ENTITY_COORDS(car, true));
			if (!helper)
			{
				ml::Notify("附近沒有可以幫忙的支援");
				return;
			}
			Start(helper, false, car, 0, zones);
			ml::Notify("支援：收到，我來搜車");
		}
		else
			Start(PLAYER::PLAYER_PED_ID(), true, car, 0, zones);
	}

	void Drop(::Ped who)
	{
		const auto it = std::find_if(g_jobs.begin(), g_jobs.end(), [&](const Job& j) { return j.who == who && !j.officer; });
		if (it == g_jobs.end())
			return;
		if (it->car && ENTITY::DOES_ENTITY_EXIST(it->car))
		{
			VEHICLE::SET_VEHICLE_DOOR_SHUT(it->car, Door(it->zones.front()), false);
			ml::Notify("支援：搜車先停在這裡，剩下的區域還沒搜");
		}
		else if (!it->car)
			ml::Notify("支援：搜身先停，還沒搜完");
		ml::Log("search: backup called away, {} left", it->car ? it->zones.size() : 1);
		g_jobs.erase(it);
	}

	void Forget()
	{
		g_cars.clear();
		g_people.clear();
		g_frisked.clear();
		g_jobs.clear();
	}

	void Tick()
	{
		for (size_t i = 0; i < g_jobs.size();)
			if (Step(g_jobs[i]))
			{
				if (!g_jobs[i].officer)
					units::Return(g_jobs[i].who);
				g_jobs.erase(g_jobs.begin() + static_cast<std::ptrdiff_t>(i));
			}
			else
				++i;
	}
}
