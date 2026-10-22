// What the police know (user's design: plate and ID checks, dispatch queries, reports):
//   - People and vehicles get a record the first time the police look at them: a person their name, date of birth,
//     driving licence, warrants and priors; a vehicle its plate, owner, registration, insurance and whether it is
//     reported stolen. Suspects' records follow who they are (suspect.cpp sets them); everyone else's are drawn at
//     random, mostly clean.
//   - A case (a callout) keeps a log of what happens; when it ends, a report is drafted from it (place, time, people,
//     evidence, suggested charges, the account) for the officer to complete and submit in the in-car computer.
//     Reports are kept in the game's save.
#include "police.hpp"

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <map>

namespace police::records
{
	using ml::Json;

	namespace
	{
		std::map<::Ped, Person> g_people;
		std::map<Vehicle, VehicleRecord> g_vehicles;
		std::vector<std::string> g_checked; // people whose ID was checked, latest last (names)

		Case g_case;
		bool g_open = false;
		std::vector<Report> g_reports;
		bool g_loaded = false;

		const char* const kFirst[] = {"Marcus", "Tyrone", "Kevin", "Luis", "Derek", "Brandon", "Carlos", "Jamal", "Travis", "Andre", "Emily", "Jessica",
		    "Maria", "Ashley", "Sarah", "Daniel", "Michael", "Chris", "Ryan", "Jose"};
		const char* const kLast[] = {"Johnson", "Ramirez", "Walker", "Nguyen", "Brooks", "Morales", "Carter", "Hughes", "Reyes", "Bennett", "Kim",
		    "Lopez", "Miller", "Davis", "Garcia", "Wilson", "Anderson", "Thomas", "Moore", "Jackson"};

		int Rand(int lo, int hi) // [lo, hi]
		{
			return MISC::GET_RANDOM_INT_IN_RANGE(lo, hi + 1);
		}

		std::string Clock()
		{
			char buf[32];
			std::snprintf(buf, sizeof buf, "%02d:%02d", CLOCK::GET_CLOCK_HOURS(), CLOCK::GET_CLOCK_MINUTES());
			return buf;
		}
		std::string Date()
		{
			char buf[32];
			std::snprintf(buf, sizeof buf, "%d/%02d/%02d", CLOCK::GET_CLOCK_YEAR(), CLOCK::GET_CLOCK_MONTH() + 1, CLOCK::GET_CLOCK_DAY_OF_MONTH());
			return buf;
		}

		Person Draw()
		{
			Person p;
			p.name = RandomName();
			char dob[16];
			std::snprintf(dob, sizeof dob, "%d/%02d/%02d", CLOCK::GET_CLOCK_YEAR() - Rand(19, 64), Rand(1, 12), Rand(1, 28));
			p.dob = dob;
			const int l = Rand(0, 99);
			p.licence = l < 85 ? "有效" : l < 93 ? "過期" : l < 97 ? "吊銷" : "無駕照";
			p.warrant = Rand(0, 999) < 15; // 1.5%
			if (p.warrant)
				p.warrantFor = Rand(0, 1) ? "未出庭" : "竊盜";
			if (Rand(0, 99) < 12)
				p.priors.push_back(Rand(0, 1) ? "輕微竊盜" : "妨害安寧");
			return p;
		}

		Json ToJson(const Person& p)
		{
			Json priors = Json::Array();
			for (const std::string& s : p.priors)
				priors.Push(s);
			return Json{{"name", p.name}, {"dob", p.dob}, {"licence", p.licence}, {"warrant", p.warrant}, {"warrantFor", p.warrantFor}, {"priors", priors}};
		}

		Json ToJson(const Report& r)
		{
			Json charges = Json::Array(), evidence = Json::Array(), log = Json::Array(), people = Json::Array(), conduct = Json::Array();
			for (const std::string& s : r.conduct)
				conduct.Push(s);
			for (const std::string& s : r.charges)
				charges.Push(s);
			for (const std::string& s : r.suggested)
				evidence.Push(s);
			for (const std::string& s : r.evidence)
				log.Push(s);
			for (const std::string& s : r.people)
				people.Push(s);
			return Json{{"id", r.id}, {"title", r.title}, {"date", r.date}, {"time", r.time}, {"place", r.place}, {"outcome", r.outcome},
			    {"people", people}, {"evidence", log}, {"suggested", evidence}, {"charges", charges}, {"narrative", r.narrative},
			    {"submitted", r.submitted}, {"score", r.score}, {"conduct", conduct}, {"conductPoints", r.conductPoints}};
		}

		Report FromJson(const Json& j)
		{
			Report r;
			r.id = j["id"].Int();
			r.title = j["title"].Str();
			r.date = j["date"].Str();
			r.time = j["time"].Str();
			r.place = j["place"].Str();
			r.outcome = j["outcome"].Str();
			for (const Json& s : j["people"].Items())
				r.people.push_back(s.Str());
			for (const Json& s : j["evidence"].Items())
				r.evidence.push_back(s.Str());
			for (const Json& s : j["suggested"].Items())
				r.suggested.push_back(s.Str());
			for (const Json& s : j["charges"].Items())
				r.charges.push_back(s.Str());
			for (const Json& s : j["conduct"].Items())
				r.conduct.push_back(s.Str());
			r.conductPoints = j["conductPoints"].Int();
			r.narrative = j["narrative"].Str();
			r.submitted = j["submitted"].Bool();
			r.score = j["score"].Int();
			return r;
		}

		void Store()
		{
			Json all = Json::Array();
			for (const Report& r : g_reports)
				all.Push(ToJson(r));
			ml::save::Set("officer_reports", all);
		}

		void Load()
		{
			if (g_loaded)
				return;
			g_loaded = true;
			g_reports.clear();
			for (const Json& j : ml::save::Get("officer_reports").Items())
				g_reports.push_back(FromJson(j));
		}
	}

	std::string Photo(Hash model)
	{
		// The vehicle shop sites' pictures (texture name = the model's name), as the property mod found them
		// (ModLoader\mods\property\data\shop_catalog.txt). Without it, or for vehicles no shop sells (police cars...),
		// no picture: the page shows a placeholder.
		static std::map<std::string, std::string> pictures;
		static bool read = false;
		if (!read)
		{
			read = true;
			std::filesystem::path mod = ml::Context().modDir;
			if (!mod.has_filename())
				mod = mod.parent_path(); // (a trailing separator)
			const std::filesystem::path file = mod.parent_path() / L"property" / L"data" / L"shop_catalog.txt";
			std::ifstream in(file);
			for (std::string line; std::getline(in, line);)
			{
				const size_t sp = line.rfind(' ');
				const size_t slash = line.rfind('/');
				if (sp == std::string::npos || slash == std::string::npos || slash < sp)
					continue;
				pictures[line.substr(slash + 1)] = line.substr(sp + 1);
			}
			if (pictures.empty())
				ml::Log("records: no vehicle pictures ({} not found): the computer shows placeholders", file.string());
		}
		const char* label = VEHICLE::GET_DISPLAY_NAME_FROM_VEHICLE_MODEL(model);
		if (!label)
			return "";
		std::string key = label;
		std::transform(key.begin(), key.end(), key.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
		const auto it = pictures.find(key);
		return it == pictures.end() ? "" : "https://gametextures/" + it->second + ".png";
	}

	std::string RandomName()
	{
		return std::string(kFirst[Rand(0, static_cast<int>(std::size(kFirst)) - 1)]) + " " + kLast[Rand(0, static_cast<int>(std::size(kLast)) - 1)];
	}

	Person& PersonOf(::Ped ped)
	{
		auto it = g_people.find(ped);
		if (it == g_people.end())
			it = g_people.emplace(ped, Draw()).first;
		return it->second;
	}

	void SetSuspect(::Ped ped, const std::string& name, float guilt, bool drunk)
	{
		Person p = Draw();
		p.name = name;
		p.warrant = guilt > 0.8f;
		p.warrantFor = p.warrant ? (guilt > 0.92f ? "持械搶劫" : "竊盜") : "";
		p.priors.clear();
		if (guilt > 0.6f)
			p.priors = {"竊盜", "持有管制藥物"};
		else if (guilt > 0.4f)
			p.priors = {"妨害安寧"};
		if (guilt > 0.7f && Rand(0, 1))
			p.licence = "吊銷";
		p.drunk = drunk;
		g_people[ped] = p;
	}

	VehicleRecord& VehicleOf(Vehicle v)
	{
		auto it = g_vehicles.find(v);
		if (it == g_vehicles.end())
		{
			VehicleRecord r;
			const char* plate = VEHICLE::GET_VEHICLE_NUMBER_PLATE_TEXT(v);
			r.plate = plate ? plate : "";
			r.plate.erase(std::remove(r.plate.begin(), r.plate.end(), ' '), r.plate.end());
			r.model = ENTITY::GET_ENTITY_MODEL(v);
			// The owner: whoever drives it, mostly.
			const ::Ped driver = VEHICLE::GET_PED_IN_VEHICLE_SEAT(v, -1, false);
			r.owner = driver && !PED::IS_PED_A_PLAYER(driver) && Rand(0, 99) < 85 ? PersonOf(driver).name : RandomName();
			const int reg = Rand(0, 99);
			r.registration = reg < 90 ? "有效" : "過期";
			r.insurance = Rand(0, 99) < 92 ? "有效" : "無";
			r.stolen = Rand(0, 999) < 8;
			it = g_vehicles.emplace(v, r).first;
		}
		return it->second;
	}

	void MarkStolen(Vehicle v)
	{
		VehicleRecord& r = VehicleOf(v);
		r.stolen = true;
		r.owner = RandomName();
	}

	Json PersonJson(const Person& p)
	{
		return ToJson(p);
	}

	Json VehicleJson(Vehicle v)
	{
		VehicleRecord& r = VehicleOf(v);
		// The owner's record: the person driving when they are the owner, a record of their own otherwise.
		Json owner;
		const ::Ped driver = VEHICLE::GET_PED_IN_VEHICLE_SEAT(v, -1, false);
		if (driver && PersonOf(driver).name == r.owner)
			owner = ToJson(PersonOf(driver));
		else
		{
			static std::map<std::string, Person> owners;
			auto it = owners.find(r.owner);
			if (it == owners.end())
			{
				Person p = Draw();
				p.name = r.owner;
				it = owners.emplace(r.owner, p).first;
			}
			owner = ToJson(it->second);
		}
		// (the text comes back in one buffer the game reuses: each copied before the next)
		const auto text = [](const char* label) {
			const char* t = label && *label ? HUD::GET_FILENAME_FOR_AUDIO_CONVERSATION(label) : nullptr;
			return t && std::string(t) != "NULL" ? std::string(t) : std::string();
		};
		const std::string model = text(VEHICLE::GET_DISPLAY_NAME_FROM_VEHICLE_MODEL(r.model));
		const std::string make = text(VEHICLE::GET_MAKE_NAME_FROM_VEHICLE_MODEL(r.model));
		int primary = 0, secondary = 0;
		VEHICLE::GET_VEHICLE_COLOURS(v, &primary, &secondary);
		return Json{{"plate", r.plate}, {"model", model}, {"make", make}, {"photo", Photo(r.model)}, {"owner", owner},
		    {"registration", r.registration}, {"insurance", r.insurance}, {"stolen", r.stolen}, {"colour", primary}};
	}

	Vehicle FindPlate(const std::string& plate)
	{
		std::string want = plate;
		want.erase(std::remove(want.begin(), want.end(), ' '), want.end());
		std::transform(want.begin(), want.end(), want.begin(), [](unsigned char c) { return static_cast<char>(std::toupper(c)); });
		for (auto& [v, r] : g_vehicles)
			if (r.plate == want && ENTITY::DOES_ENTITY_EXIST(v))
				return v;
		// Vehicles around the officer that nobody has looked at yet.
		int64_t buf[1 + 40] = {40};
		const int n = PED::GET_PED_NEARBY_VEHICLES(PLAYER::PLAYER_PED_ID(), reinterpret_cast<Any*>(buf));
		for (int i = 0; i < n && i < 40; ++i)
			if (const Vehicle v = static_cast<Vehicle>(buf[1 + i]); v && VehicleOf(v).plate == want)
				return v;
		return 0;
	}

	const Person* FindName(const std::string& name)
	{
		for (auto& [ped, p] : g_people)
			if (p.name == name)
				return &p;
		return nullptr;
	}

	void Checked(::Ped ped)
	{
		const std::string& n = PersonOf(ped).name;
		g_checked.erase(std::remove(g_checked.begin(), g_checked.end(), n), g_checked.end());
		g_checked.push_back(n);
	}

	Json CheckedPeople()
	{
		Json out = Json::Array();
		for (auto it = g_checked.rbegin(); it != g_checked.rend(); ++it)
			if (const Person* p = FindName(*it))
				out.Push(ToJson(*p));
		return out;
	}

	// ---- cases and reports

	void BeginCase(const std::string& title, const std::string& place)
	{
		g_case = {};
		g_case.title = title;
		g_case.place = place;
		g_case.date = Date();
		g_case.time = Clock();
		g_open = true;
		Note("受理：" + title + "（" + place + "）");
	}

	bool CaseOpen()
	{
		return g_open;
	}

	void Note(const std::string& what)
	{
		if (g_open)
			g_case.log.push_back(Clock() + " " + what);
	}

	void Evidence(const std::string& item)
	{
		if (!g_open)
			return;
		if (std::find(g_case.evidence.begin(), g_case.evidence.end(), item) == g_case.evidence.end())
			g_case.evidence.push_back(item);
	}

	void Involved(const std::string& person)
	{
		if (g_open && std::find(g_case.people.begin(), g_case.people.end(), person) == g_case.people.end())
			g_case.people.push_back(person);
	}

	void Charge(const std::string& charge)
	{
		if (g_open && std::find(g_case.suggested.begin(), g_case.suggested.end(), charge) == g_case.suggested.end())
			g_case.suggested.push_back(charge);
	}

	std::vector<std::string> Charges()
	{
		return g_open ? g_case.suggested : std::vector<std::string>{};
	}

	void Conduct(int points, const std::string& why)
	{
		if (!g_open)
			return;
		g_case.conductPoints += points;
		g_case.conduct.push_back((points >= 0 ? "+" : "") + std::to_string(points) + " " + why);
		ml::Log("records: conduct {:+} {}", points, why);
	}

	int EndCase(const std::string& outcome)
	{
		if (!g_open)
			return 0;
		Load();
		Note("結案：" + outcome);
		Report r;
		r.id = g_reports.empty() ? 1 : g_reports.back().id + 1;
		r.title = g_case.title;
		r.date = g_case.date;
		r.time = g_case.time;
		r.place = g_case.place;
		r.outcome = outcome;
		r.people = g_case.people;
		r.evidence = g_case.evidence;
		r.suggested = g_case.suggested;
		r.charges = g_case.suggested; // the draft starts with what the evidence supports
		r.conduct = g_case.conduct;
		r.conductPoints = g_case.conductPoints;
		// The account, from the log.
		const std::string me = look::Name().empty() ? "員警" : "員警 " + look::Name();
		std::string text = me + " 於 " + r.date + " " + r.time + " 受理「" + r.title + "」。\n";
		for (const std::string& line : g_case.log)
			text += line + "\n";
		r.narrative = text;
		g_reports.push_back(r);
		Store();
		g_open = false;
		ml::Log("records: report {} drafted ({} log lines, {} pieces of evidence)", r.id, g_case.log.size(), r.evidence.size());
		return r.id;
	}

	Json Reports()
	{
		Load();
		Json out = Json::Array();
		for (auto it = g_reports.rbegin(); it != g_reports.rend(); ++it)
			out.Push(ToJson(*it));
		return out;
	}

	int Drafts()
	{
		Load();
		return static_cast<int>(std::count_if(g_reports.begin(), g_reports.end(), [](const Report& r) { return !r.submitted; }));
	}

	Json CurrentCase()
	{
		if (!g_open)
			return Json();
		Json log = Json::Array(), ev = Json::Array();
		for (const std::string& s : g_case.log)
			log.Push(s);
		for (const std::string& s : g_case.evidence)
			ev.Push(s);
		return Json{{"title", g_case.title}, {"place", g_case.place}, {"log", log}, {"evidence", ev}};
	}

	Json Submit(int id, const std::vector<std::string>& charges, const std::string& narrative)
	{
		Load();
		for (Report& r : g_reports)
			if (r.id == id && !r.submitted)
			{
				r.charges = charges;
				r.narrative = narrative;
				r.submitted = true;
				// The quality the department sees: charges backed by the evidence, none left out, a real account, and how
				// the officer did.
				int score = 60;
				for (const std::string& c : r.charges)
					score += std::find(r.suggested.begin(), r.suggested.end(), c) != r.suggested.end() ? 8 : -10;
				for (const std::string& c : r.suggested)
					if (std::find(r.charges.begin(), r.charges.end(), c) == r.charges.end())
						score -= 5;
				score += std::min(20, static_cast<int>(narrative.size()) / 60);
				score += r.conductPoints; // how the officer did (the transport)
				r.score = std::clamp(score, 0, 100);
				Store();
				ml::Log("records: report {} submitted, score {}", r.id, r.score);
				return Json{{"ok", true}, {"score", r.score}};
			}
		return Json{{"ok", false}};
	}

	void Loaded()
	{
		g_loaded = false;
		Load();
	}

	void Forget()
	{
		g_people.clear();
		g_vehicles.clear();
		g_checked.clear();
		g_open = false;
	}
}
