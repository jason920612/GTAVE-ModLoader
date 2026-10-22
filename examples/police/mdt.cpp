// The in-car computer (user's design: a computer in the patrol car, but picking a vehicle from a list of those around,
// with its picture, instead of typing plates): a page of the in-game browser (mods\police\web\mdt.lspd) with the
// vehicles around, a plate or name search (the plate reader's last read filled in), the people whose ID was checked,
// and the case under way and its reports (complete and submit the drafts), and the officer's duty (shift.cpp: the shift,
// the week's roster, payslips, attendance) and career (career.cpp: the promotion's requirements and the exam, taken at
// the station). Away from the car the radio asks dispatch
// the same questions; the answer comes back after a few seconds and goes into the computer too.
//
// The plate reader: in a police car, the plate of the vehicle ahead (within 30 m, straight ahead) is read and shown by
// the minimap; a stolen vehicle or an owner with a warrant raises an alert.
#include "police.hpp"

#include <algorithm>
#include <atomic>
#include <cmath>
#include <set>
#include <string>
#include <vector>

namespace police::mdt
{
	namespace
	{
		using ai::Distance;

		std::atomic<bool> g_openRequested = false;
		Vehicle g_ahead = 0;
		std::string g_aheadPlate;
		std::set<Vehicle> g_alerted;
		uint64_t g_nextRead = 0;

		struct Query
		{
			uint64_t at = 0;
			Vehicle car = 0;
			::Ped ped = 0;
		};
		std::vector<Query> g_queries;

		bool InPoliceCar()
		{
			const ::Ped p = PLAYER::PLAYER_PED_ID();
			return PED::IS_PED_IN_ANY_VEHICLE(p, false) && VEHICLE::GET_VEHICLE_CLASS(PED::GET_VEHICLE_PED_IS_IN(p, false)) == 18;
		}

		Vehicle Mine()
		{
			const ::Ped p = PLAYER::PLAYER_PED_ID();
			return PED::IS_PED_IN_ANY_VEHICLE(p, false) ? PED::GET_VEHICLE_PED_IS_IN(p, false) : 0;
		}

		std::string Model(Vehicle v)
		{
			const char* label = VEHICLE::GET_DISPLAY_NAME_FROM_VEHICLE_MODEL(ENTITY::GET_ENTITY_MODEL(v));
			const char* text = label ? HUD::GET_FILENAME_FOR_AUDIO_CONVERSATION(label) : nullptr;
			return text && std::string(text) != "NULL" ? text : (label ? label : "");
		}

		// What dispatch says about a vehicle (one line), and the case log.
		std::string Summary(Vehicle v)
		{
			const records::VehicleRecord& r = records::VehicleOf(v);
			const ml::Json j = records::VehicleJson(v);
			std::string s = "車牌 " + r.plate + "，" + Model(v) + "，車主 " + r.owner;
			if (r.stolen)
				s += "，已報失竊";
			if (r.registration != "有效")
				s += "，行照" + r.registration;
			if (r.insurance != "有效")
				s += "，無保險";
			if (j["owner"]["warrant"].Bool())
				s += "，車主有通緝（" + j["owner"]["warrantFor"].Str() + "）";
			if (!r.stolen && r.registration == "有效" && r.insurance == "有效" && !j["owner"]["warrant"].Bool())
				s += "，無異常";
			return s;
		}

		std::string Summary(const records::Person& p)
		{
			std::string s = p.name + "，生日 " + p.dob + "，駕照" + p.licence;
			if (p.warrant)
				s += "，有通緝（" + p.warrantFor + "）";
			if (!p.priors.empty())
			{
				s += "，前科：";
				for (size_t i = 0; i < p.priors.size(); ++i)
					s += (i ? "、" : "") + p.priors[i];
			}
			if (!p.warrant && p.priors.empty())
				s += "，無紀錄";
			return s;
		}

		void Logged(Vehicle v)
		{
			const records::VehicleRecord& r = records::VehicleOf(v);
			records::Note("查詢車牌：" + Summary(v));
			if (r.stolen && suspect::OwnerOf(v))
				records::Charge("竊盜（車輛）");
		}

		void LoggedPerson(::Ped ped)
		{
			const records::Person& p = records::PersonOf(ped);
			records::Checked(ped);
			records::Note("查詢人員：" + Summary(p));
			ai::Profile pr;
			if (!suspect::ProfileOf(ped, pr))
				return;
			records::Involved(p.name + "（嫌犯）");
			if (p.warrant)
				records::Charge("通緝犯（" + p.warrantFor + "）");
			if ((p.licence == "吊銷" || p.licence == "無駕照") && suspect::Drove(ped))
				records::Charge(p.licence == "吊銷" ? "吊銷駕照駕駛" : "無照駕駛");
			if (pr.impairment == ai::Impairment::Drunk && suspect::Drove(ped))
				records::Charge("酒後駕駛");
		}

		// ---- the page's functions
		ml::Json Nearby()
		{
			ml::Json out = ml::Json::Array();
			const ::Ped me = PLAYER::PLAYER_PED_ID();
			const Vector3 at = ENTITY::GET_ENTITY_COORDS(me, true);
			int64_t buf[1 + 40] = {40};
			const int n = PED::GET_PED_NEARBY_VEHICLES(me, reinterpret_cast<Any*>(buf));
			std::vector<std::pair<float, Vehicle>> list;
			for (int i = 0; i < n && i < 40; ++i)
			{
				const Vehicle v = static_cast<Vehicle>(buf[1 + i]);
				if (!v || v == Mine() || !ENTITY::DOES_ENTITY_EXIST(v))
					continue;
				const Hash m = ENTITY::GET_ENTITY_MODEL(v);
				if (!(VEHICLE::IS_THIS_MODEL_A_CAR(m) || VEHICLE::IS_THIS_MODEL_A_BIKE(m) || VEHICLE::IS_THIS_MODEL_A_QUADBIKE(m)))
					continue;
				const float d = Distance(at, ENTITY::GET_ENTITY_COORDS(v, true));
				if (d < 90.0f)
					list.emplace_back(d, v);
			}
			std::sort(list.begin(), list.end());
			for (const auto& [d, v] : list)
			{
				const records::VehicleRecord& r = records::VehicleOf(v);
				out.Push(ml::Json{{"id", static_cast<int>(v)}, {"plate", r.plate}, {"model", Model(v)}, {"photo", records::Photo(r.model)},
				    {"distance", static_cast<int>(d)}, {"ahead", v == g_ahead}, {"suspect", suspect::OwnerOf(v) != 0},
				    {"occupied", VEHICLE::GET_PED_IN_VEHICLE_SEAT(v, -1, false) != 0}});
			}
			return out;
		}

		ml::Json VehicleById(int id)
		{
			const Vehicle v = static_cast<Vehicle>(id);
			if (!v || !ENTITY::DOES_ENTITY_EXIST(v))
				return ml::Json{{"error", "找不到這台車"}};
			Logged(v);
			ml::Json j = records::VehicleJson(v);
			j["id"] = id;
			return j;
		}

		ml::Json ByPlate(std::string plate)
		{
			const Vehicle v = records::FindPlate(plate);
			if (!v)
				return ml::Json{{"error", "查無此車牌（附近沒有這台車的資料）"}};
			return VehicleById(static_cast<int>(v));
		}

		ml::Json ByName(std::string name)
		{
			const records::Person* p = records::FindName(name);
			if (!p)
				return ml::Json{{"error", "查無此人"}};
			records::Note("查詢人員：" + Summary(*p));
			return records::PersonJson(*p);
		}

		ml::Json State()
		{
			return ml::Json{{"officer", look::Name()}, {"inCar", InPoliceCar()}, {"ahead", g_ahead ? static_cast<int>(g_ahead) : 0}, {"aheadPlate", g_aheadPlate},
			    {"case", records::CurrentCase()}};
		}

		ml::Json Charges()
		{
			ml::Json out = ml::Json::Array();
			for (const char* c : {"竊盜（車輛）", "收受贓物", "駕車逃避警察攔查", "危險駕駛", "拒捕", "妨害公務", "以武器攻擊警察", "強盜（劫車）",
			         "非法持有武器", "持有危險器械", "持有第一級管制藥物", "持有第二級管制藥物", "持有第三級管制藥物", "持有第四級管制藥物",
			         "車內有開封酒類", "酒後駕駛", "無照駕駛", "吊銷駕照駕駛", "通緝犯（竊盜）", "通緝犯（持械搶劫）", "通緝犯（未出庭）"})
				out.Push(c);
			return out;
		}

		void ReadPlates()
		{
			const ::Ped me = PLAYER::PLAYER_PED_ID();
			const Vehicle mine = Mine();
			g_ahead = 0;
			if (!mine || VEHICLE::GET_VEHICLE_CLASS(mine) != 18)
				return;
			const Vector3 at = ENTITY::GET_ENTITY_COORDS(mine, true);
			const Vector3 fwd = ENTITY::GET_ENTITY_FORWARD_VECTOR(mine);
			int64_t buf[1 + 20] = {20};
			const int n = PED::GET_PED_NEARBY_VEHICLES(me, reinterpret_cast<Any*>(buf));
			float best = 30.0f;
			for (int i = 0; i < n && i < 20; ++i)
			{
				const Vehicle v = static_cast<Vehicle>(buf[1 + i]);
				if (!v || v == mine || !ENTITY::DOES_ENTITY_EXIST(v))
					continue;
				const Vector3 p = ENTITY::GET_ENTITY_COORDS(v, true);
				const float dx = p.x - at.x, dy = p.y - at.y;
				const float ahead = dx * fwd.x + dy * fwd.y;
				const float side = std::abs(dx * fwd.y - dy * fwd.x);
				if (ahead > 3.0f && ahead < best && side < 3.5f)
					best = ahead, g_ahead = v;
			}
			if (!g_ahead)
				return;
			const records::VehicleRecord& r = records::VehicleOf(g_ahead);
			g_aheadPlate = r.plate;
			if (g_alerted.insert(g_ahead).second)
			{
				const ml::Json j = records::VehicleJson(g_ahead);
				std::string why;
				if (r.stolen)
					why = "失竊車輛";
				else if (j["owner"]["warrant"].Bool())
					why = "車主有通緝";
				if (!why.empty())
				{
					AUDIO::PLAY_SOUND_FRONTEND(-1, "Beep_Red", "DLC_HEIST_HACKING_SNAKE_SOUNDS", true);
					ml::Notify("車牌辨識警示：{}（{}，{}）", r.plate, Model(g_ahead), why);
					ml::Log("mdt: plate reader alert {} ({})", r.plate, why);
				}
			}
		}

		void DrawReader()
		{
			if (!g_ahead || !InPoliceCar())
				return;
			const records::VehicleRecord& r = records::VehicleOf(g_ahead);
			const bool flag = r.stolen;
			HUD::SET_TEXT_FONT(0);
			HUD::SET_TEXT_SCALE(0.0f, 0.36f);
			HUD::SET_TEXT_COLOUR(flag ? 255 : 230, flag ? 90 : 230, flag ? 90 : 230, 255);
			HUD::SET_TEXT_OUTLINE();
			HUD::BEGIN_TEXT_COMMAND_DISPLAY_TEXT("STRING");
			HUD::ADD_TEXT_COMPONENT_SUBSTRING_PLAYER_NAME((std::string("車牌辨識 ") + r.plate + (flag ? "  失竊" : "")).c_str());
			HUD::END_TEXT_COMMAND_DISPLAY_TEXT(0.17f, 0.895f, 0);
		}
	}

	void Build()
	{
		ml::Hotkey("hkMdt", "車上電腦", 0x79 /* F10 */, [] { g_openRequested = true; });
		if (!ml::web::Available())
		{
			ml::Log("mdt: no in-game browser: the in-car computer is not available (the radio still answers)");
			return;
		}
		ml::web::Function("mdt.state", [] { return State(); });
		ml::web::Function("mdt.nearby", [] { return Nearby(); });
		ml::web::Function("mdt.vehicle", [](int id) { return VehicleById(id); });
		ml::web::Function("mdt.plate", [](std::string plate) { return ByPlate(plate); });
		ml::web::Function("mdt.person", [](std::string name) { return ByName(name); });
		ml::web::Function("mdt.people", [] { return records::CheckedPeople(); });
		ml::web::Function("mdt.reports", [] { return records::Reports(); });
		ml::web::Function("mdt.charges", [] { return Charges(); });
		ml::web::Function("mdt.shift", [] { return shift::Json(); });
		ml::web::Function("mdt.career", [] { return career::Json(); });
		ml::web::Function("mdt.colleagues", [] { return colleagues::Json(); });
		ml::web::Function("mdt.chat", [](int id) { return colleagues::Chat(id); });
		ml::web::Function("mdt.say", [](int id, std::string text) { return colleagues::Say(id, text); });
		ml::web::Function("mdt.examStart", [] { return career::StartExam(); });
		ml::web::Function("mdt.examSubmit", [](std::vector<int> answers) { return career::SubmitExam(answers); });
		ml::web::Function("mdt.submit", [](int id, std::vector<std::string> charges, std::string narrative) { return records::Submit(id, charges, narrative); });
	}

	void Open()
	{
		if (!InPoliceCar())
		{
			ml::Notify("要坐在警車上才能用車上電腦（車外可以用 F9「向調度查詢」）");
			return;
		}
		if (!ml::web::Available())
		{
			ml::Notify("沒有遊戲內瀏覽器，車上電腦無法使用");
			return;
		}
		ml::web::Open("https://mdt.lspd/");
	}

	void RadioPlate(Vehicle v)
	{
		if (!v)
			return;
		ml::Notify("無線電：調度，請查車牌 {}", records::VehicleOf(v).plate);
		g_queries.push_back({ml::TickMs() + 3500 + static_cast<uint64_t>(ai::Random() * 3000.0f), v, 0});
	}

	void RadioPerson(::Ped ped)
	{
		if (!ped)
			return;
		ml::Notify("無線電：調度，請查 {}", records::PersonOf(ped).name);
		g_queries.push_back({ml::TickMs() + 4000 + static_cast<uint64_t>(ai::Random() * 3000.0f), 0, ped});
	}

	Vehicle Ahead()
	{
		return g_ahead;
	}

	void Forget()
	{
		g_ahead = 0;
		g_alerted.clear();
		g_queries.clear();
	}

	void Tick()
	{
		if (!officer::IsPlayer())
			return;
		const uint64_t now = ml::TickMs();
		if (g_openRequested.exchange(false))
			Open();
		if (now >= g_nextRead)
		{
			g_nextRead = now + 300;
			ReadPlates();
		}
		DrawReader();
		for (size_t i = 0; i < g_queries.size();)
		{
			const Query q = g_queries[i];
			if (now < q.at)
			{
				++i;
				continue;
			}
			g_queries.erase(g_queries.begin() + static_cast<std::ptrdiff_t>(i));
			AUDIO::PLAY_SOUND_FRONTEND(-1, "Event_Message_Purple", "GTAO_FM_Events_Soundset", true);
			if (q.car && ENTITY::DOES_ENTITY_EXIST(q.car))
			{
				ml::Notify("調度：{}", Summary(q.car));
				ml::Log("mdt: radio answer: {}", Summary(q.car));
				Logged(q.car);
			}
			else if (q.ped && ENTITY::DOES_ENTITY_EXIST(q.ped))
			{
				ml::Notify("調度：{}", Summary(records::PersonOf(q.ped)));
				ml::Log("mdt: radio answer: {}", Summary(records::PersonOf(q.ped)));
				LoggedPerson(q.ped);
			}
		}
	}
}
