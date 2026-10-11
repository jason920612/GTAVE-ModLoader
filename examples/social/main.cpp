// Social mod: the Bleeter and Lifeinvader websites of the loader's browser (web\www.bleeter.biz, web\www.lifeinvader.com;
// research/phase0.md §39).
//
// The posts are the pages' own (written for this mod); which ones show depends on the story and on what the player
// does. This mod tells the pages:
//   - the story missions passed and when (game time, recorded when a mission is passed; -1 for those passed before
//     the mod saw them), from the game's mission table (Global 114990+9094+330, 6 slots each: +0 passed, +3 order);
//   - events it notices, kept in the save: police chases the player escaped, purchases at the vehicle sites and
//     Dynasty 8, hospital and bail bills (from the game's bank log, Global 114990+20573+233), big stock moves.
//
// Pages call:
//   social.feed() -> { character, now, weather, missions: [ { id, script, passed, order, t } ], events: [ { t, type, c, a, b, n } ] }
//     event types: chase (a zone, b vehicle, n stars), buy (a shop, b vehicles/property, n $), hospital (a name, n $),
//                  arrest (a station, n $), stock (a ticker, b company, n change in hundredths of a percent)
#include <algorithm>
#include <atomic>
#include <cmath>
#include <map>
#include <vector>

#include <modloader/game.hpp>
#include <modloader/json.hpp>

ML_MOD_INFO("Social", "0.1.0", "ModLoader", "The Bleeter and Lifeinvader websites")

namespace
{
	std::string Text(const char* label)
	{
		if (!label || !*label || !HUD::DOES_TEXT_LABEL_EXIST(label))
			return {};
		std::string s = HUD::GET_FILENAME_FOR_AUDIO_CONVERSATION(label);
		for (size_t at; (at = s.find("\xC2\xB5")) != std::string::npos;) // the game's non-breaking space
			s.replace(at, 2, " ");
		return s;
	}

	int64_t DaysFromCivil(int64_t y, unsigned m, unsigned d)
	{
		y -= m <= 2;
		const int64_t era = (y >= 0 ? y : y - 399) / 400;
		const unsigned yoe = static_cast<unsigned>(y - era * 400);
		const unsigned doy = (153 * (m + (m > 2 ? -3 : 9)) + 2) / 5 + d - 1;
		const unsigned doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
		return era * 146097 + static_cast<int64_t>(doe) - 719468;
	}
	// Game minutes since 2000-01-01 00:00.
	int64_t GameMinute()
	{
		const int64_t days = DaysFromCivil(CLOCK::GET_CLOCK_YEAR(), static_cast<unsigned>(CLOCK::GET_CLOCK_MONTH() + 1),
		                         static_cast<unsigned>(CLOCK::GET_CLOCK_DAY_OF_MONTH())) - DaysFromCivil(2000, 1, 1);
		return (days * 24 + CLOCK::GET_CLOCK_HOURS()) * 60 + CLOCK::GET_CLOCK_MINUTES();
	}

	// ---- story missions --------------------------------------------------------------------------

	constexpr int kMissions = 94;
	ml::Global Mission(int i) { return (ml::Global(114990) + (9094 + 330)).At(i, 6); }
	std::string Script(int i) { return ml::Global(93274).At(i, 34).Text(); }

	// ---- events --------------------------------------------------------------------------------------

	struct Event
	{
		int64_t t;
		std::string type;
		int c;
		std::string a, b;
		int64_t n;
	};
	constexpr size_t kMaxEvents = 400;

	struct State
	{
		std::map<int, int64_t> passed; // mission -> game minute (-1 unknown)
		std::vector<Event> events;
	};
	State g;
	std::atomic_bool g_reload = true;

	void Save()
	{
		ml::Json passed = ml::Json::Object();
		for (const auto& [id, t] : g.passed)
			passed[std::to_string(id)] = t;
		ml::Json events = ml::Json::Array();
		for (const Event& e : g.events)
			events.Push(ml::Json::Array({e.t, e.type, e.c, e.a, e.b, e.n}));
		ml::save::Set("social", {{"passed", passed}, {"events", events}});
	}

	void Load()
	{
		g = {};
		const ml::Json j = ml::save::Get("social");
		for (const auto& [id, t] : j["passed"].Members())
			g.passed[std::stoi(id)] = t.Int64();
		for (const ml::Json& e : j["events"].Items())
			g.events.push_back({e[0].Int64(), e[1].Str(), e[2].Int(), e[3].Str(), e[4].Str(), e[5].Int64()});
	}

	void AddEvent(Event e)
	{
		ml::Log("event {} {} {} {} {}", e.type, e.c, e.a, e.b, e.n);
		g.events.push_back(std::move(e));
		if (g.events.size() > kMaxEvents)
			g.events.erase(g.events.begin(), g.events.begin() + static_cast<ptrdiff_t>(g.events.size() - kMaxEvents));
		Save();
	}

	// Missions passed since the last look. The first look after a load takes what is already passed as "before".
	void TrackMissions(bool first)
	{
		bool changed = false;
		for (int i = 0; i < kMissions; ++i)
			if (Mission(i).Field(0).Int() == 1 && !g.passed.contains(i))
			{
				g.passed[i] = first ? -1 : GameMinute();
				changed = true;
				if (!first)
					ml::Log("mission {} ({}) passed", i, Script(i));
			}
		if (changed)
			Save();
	}

	// ---- the bank log: purchases, hospital and bail bills -----------------------------------------

	ml::Global BankLog(int c) { return (ml::Global(114990) + (20573 + 233)).At(c, 69); }
	int g_logCount[3] = {-1, -1, -1};

	void TrackBank(int c)
	{
		const int count = BankLog(c).Field(0).Int();
		if (g_logCount[c] < 0 || count < g_logCount[c])
		{
			g_logCount[c] = count;
			return;
		}
		const int fresh = std::min(count - g_logCount[c], 11);
		g_logCount[c] = count;
		const int next = BankLog(c).Field(1).Int();
		for (int k = fresh; k >= 1; --k)
		{
			const ml::Global e = (BankLog(c) + 2).At(((next - k) % 11 + 11) % 11, 6);
			if (e.Field(0).Int() != 0) // received
				continue;
			const int payee = e.Field(1).Int(), amount = e.Field(2).Int();
			static const std::map<int, std::pair<const char*, const char*>> kPayees = {
			    {85, {"ACCNA_CARSITE", "Legendary Motorsport"}}, {86, {"ACCNA_ARMYSITE", "Warstock Cache & Carry"}},
			    {87, {"ACCNA_PLANESITE", "Elitás Travel"}}, {88, {"ACCNA_BOATSITE", "Dock Tease"}}, {89, {"ACCNA_BIKESITE", "Pedal and Metal"}},
			    {90, {"ACCNA_AUTOSITE", "Southern San Andreas Super Autos"}}, {130, {"", "朝代 8 房地產"}}};
			const auto name = [](const char* label, const char* fallback) {
				const std::string s = Text(label);
				return s.empty() ? std::string(fallback) : s;
			};
			if (const auto it = kPayees.find(payee); it != kPayees.end())
				AddEvent({GameMinute(), "buy", c, name(it->second.first, it->second.second), payee == 130 ? "property" : "vehicles", amount});
			static const std::map<int, const char*> kHospitals = {
			    {8, "ACCNA_LSANH"}, {125, "ACCNA_H_RH"}, {126, "ACCNA_H_SC"}, {127, "ACCNA_H_DT"}, {128, "ACCNA_H_SS"}, {129, "ACCNA_H_PB"}};
			static const std::map<int, const char*> kStations = {{118, "ACCNA_PD_VB"}, {119, "ACCNA_PD_SC"}, {120, "ACCNA_PD_DT"},
			    {121, "ACCNA_PD_RH"}, {122, "ACCNA_PD_SS"}, {123, "ACCNA_PD_PB"}, {124, "ACCNA_PD_HW"}};
			if (const auto h = kHospitals.find(payee); h != kHospitals.end())
				AddEvent({GameMinute(), "hospital", c, name(h->second, "醫院"), "", amount});
			else if (const auto s = kStations.find(payee); s != kStations.end())
				AddEvent({GameMinute(), "arrest", c, name(s->second, "警察局"), "", amount});
		}
	}

	// ---- police chases -------------------------------------------------------------------------------

	struct Chase
	{
		bool on = false;
		int stars = 0;
		std::string zone, vehicle;
		bool caught = false;
	} g_chase;

	std::string ZoneHere()
	{
		const Vector3 p = ENTITY::GET_ENTITY_COORDS(PLAYER::PLAYER_PED_ID(), true);
		return Text(ZONE::GET_NAME_OF_ZONE(p.x, p.y, p.z));
	}

	void TrackChase()
	{
		const Player player = PLAYER::PLAYER_ID();
		const Ped ped = PLAYER::PLAYER_PED_ID();
		const int stars = PLAYER::GET_PLAYER_WANTED_LEVEL(player);
		if (stars > 0)
		{
			if (!g_chase.on)
				g_chase = {true, 0, ZoneHere(), "", false};
			if (stars > g_chase.stars)
				g_chase.stars = stars;
			if (g_chase.vehicle.empty() && PED::IS_PED_IN_ANY_VEHICLE(ped, false))
				g_chase.vehicle = Text(VEHICLE::GET_DISPLAY_NAME_FROM_VEHICLE_MODEL(ENTITY::GET_ENTITY_MODEL(PED::GET_VEHICLE_PED_IS_IN(ped, false))));
			if (PLAYER::IS_PLAYER_BEING_ARRESTED(player, true) || ENTITY::IS_ENTITY_DEAD(ped, false))
				g_chase.caught = true;
			return;
		}
		if (g_chase.on && !g_chase.caught && g_chase.stars >= 2 && !ENTITY::IS_ENTITY_DEAD(ped, false))
			AddEvent({GameMinute(), "chase", ml::game::CharacterIndex(), g_chase.zone, g_chase.vehicle, g_chase.stars});
		g_chase = {};
	}

	// ---- stock moves (the game's stock table, Global 57379, 36 slots: +4 ticker label, +34 change %) --------

	// At most three a game day, each stock at most once a game day (judged by the saved events, so restarts and
	// loads do not repeat them).
	void TrackStocks()
	{
		const int64_t now = GameMinute();
		int today = 0;
		for (const Event& e : g.events)
			today += e.type == "stock" && e.t > now - 1440;
		const ml::Global table{57379};
		for (int id = 0; id < table.Size() && id < 80 && today < 3; ++id)
		{
			const ml::Global s = table.At(id, 36);
			const float pct = s.Field(34).Float();
			if (std::fabs(pct) < 8.0f || s.Field(9).Float() <= 0)
				continue;
			const std::string ticker = Text(s.Field(4).Text().c_str());
			if (std::any_of(g.events.begin(), g.events.end(), [&](const Event& e) { return e.type == "stock" && e.a == ticker && e.t > now - 1440; }))
				continue;
			++today;
			AddEvent({now, "stock", -1, ticker, Text(s.Text().c_str()), static_cast<int64_t>(std::lround(pct * 100))});
		}
	}

	// ---- pages -----------------------------------------------------------------------------------------

	const char* Weather()
	{
		static const std::pair<const char*, Hash> kinds[] = {
		    {"clear", MISC::GET_HASH_KEY("CLEAR")}, {"sunny", MISC::GET_HASH_KEY("EXTRASUNNY")}, {"clouds", MISC::GET_HASH_KEY("CLOUDS")},
		    {"overcast", MISC::GET_HASH_KEY("OVERCAST")}, {"rain", MISC::GET_HASH_KEY("RAIN")}, {"clearing", MISC::GET_HASH_KEY("CLEARING")},
		    {"thunder", MISC::GET_HASH_KEY("THUNDER")}, {"smog", MISC::GET_HASH_KEY("SMOG")}, {"fog", MISC::GET_HASH_KEY("FOGGY")},
		    {"snow", MISC::GET_HASH_KEY("SNOW")}, {"snow", MISC::GET_HASH_KEY("XMAS")}};
		const Hash now = MISC::GET_PREV_WEATHER_TYPE_HASH_NAME();
		for (const auto& [name, hash] : kinds)
			if (hash == now)
				return name;
		return "clear";
	}

	ml::Json Feed()
	{
		ml::Json missions = ml::Json::Array();
		for (int i = 0; i < kMissions; ++i)
		{
			const auto it = g.passed.find(i);
			missions.Push({{"id", i}, {"script", Script(i)}, {"passed", it != g.passed.end()}, {"order", Mission(i).Field(3).Int()},
			    {"t", it != g.passed.end() ? it->second : -1}});
		}
		ml::Json events = ml::Json::Array();
		for (const Event& e : g.events)
			events.Push({{"t", e.t}, {"type", e.type}, {"c", e.c}, {"a", e.a}, {"b", e.b}, {"n", e.n}});
		return {{"character", ml::game::CharacterIndex()}, {"now", GameMinute()}, {"weather", Weather()}, {"missions", missions}, {"events", events}};
	}
}

extern "C" __declspec(dllexport) int MLOnLoad(const MLApi* api, const MLContext* ctx)
{
	ml::Init(api, ctx);
	if (!ml::web::Available() || !ml::save::Available())
	{
		ml::LogError("this loader has no web browser or save data; the social mod needs them");
		return 0;
	}
	ml::web::Function("social.feed", [] { return Feed(); });
	ml::game::On(ml::game::Event::SaveLoading, [] { g_reload = true; });
	return 1;
}

extern "C" __declspec(dllexport) void MLMain()
{
	uint64_t nextSlow = 0;
	for (;;)
	{
		if (ml::game::CharacterIndex() >= 0 && !ml::game::IsLoadingScreen())
		{
			const bool first = g_reload.exchange(false);
			if (first)
			{
				Load();
				std::fill(std::begin(g_logCount), std::end(g_logCount), -1);
				g_chase = {};
			}
			TrackChase();
			if (first || ml::TickMs() >= nextSlow)
			{
				nextSlow = ml::TickMs() + 2000;
				TrackMissions(first);
				for (int c = 0; c < 3; ++c)
					TrackBank(c);
				TrackStocks();
			}
		}
		ml::Wait(250);
	}
}
