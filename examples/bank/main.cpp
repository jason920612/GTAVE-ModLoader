// Bank mod: the Maze Bank, Fleeca and Bank of Liberty websites of the loader's browser (web\www.maze-bank.com,
// web\www.fleeca.com, web\www.thebankofliberty.com; research/phase0.md §38).
//
// Each story character banks at one of them (Michael Maze Bank, Franklin Fleeca, Trevor Bank of Liberty). The balance
// is the character's money. The game keeps the last 11 payments of each character (appinternet's bank function,
// ml::game::Pay, logs them, without dates): this mod watches that log and the balance, dates every change with the
// game clock and keeps the history in the save (ml::save, per character). Money that moves without a log entry
// (mission rewards paid directly, pickups, cheats) shows as "other".
//
// Pages call:
//   bank.account(bank) -> { bank, character, owner, customer, balance, now, entries: [ { t, payee, name, category, amount, balance } ] }
//                         (entries newest first; t = game minutes since 2000-01-01, -1 when unknown)
#include <algorithm>
#include <atomic>
#include <cstring>
#include <format>
#include <vector>

#include <modloader/game.hpp>
#include <modloader/json.hpp>

ML_MOD_INFO("Bank", "0.1.0", "ModLoader", "The Maze Bank, Fleeca and Bank of Liberty websites")

namespace
{
	// ---- the game's bank data ------------------------------------------------------------------
	// Global 114990+20573+233: per character 69 slots: +0 entries ever logged, +1 next slot, +2 eleven entries of
	// 6 slots (+0 0 paid / 1 received, +1 payee, +2 amount, +3..+5 a copy).
	constexpr int kGameSlots = 11;
	ml::Global Log(int c) { return (ml::Global(114990) + (20573 + 233)).At(c, 69); }
	ml::Global GameEntry(int c, int i) { return (Log(c) + 2).At(i, 6); }

	// ---- payees: the game's names (ACCNA_* / shop labels) and a category for the pages --------
	struct Payee
	{
		int id;
		const char* label;    // game text label
		const char* fallback; // when the label has no text
		const char* category; // invest people jobs shopping weapons vehicles property fun bills other
	};
	constexpr Payee kPayees[] = {
	    {2, "ACCNA_BROKERA", "股票買賣", "invest"}, {3, "ACCNA_MIKE", "麥可", "people"}, {4, "ACCNA_CBELL", "", "shopping"},
	    {5, "ACCNA_WHIZZ", "", "bills"}, {6, "ACCNA_MCHON", "", "fun"}, {7, "ACCNA_DSACH", "", "shopping"}, {8, "ACCNA_LSANH", "", "bills"},
	    {9, "ACCNA_CRAPKI", "", "shopping"}, {10, "ACCNA_VCLEAN", "", "vehicles"}, {11, "ACCNA_CSUX", "", "bills"}, {12, "ACCNA_VBEU", "", "shopping"},
	    {13, "ACCNA_ANAT", "", "weapons"}, {14, "ACCNA_BAHAMA", "", "fun"}, {15, "ACCNA_BAR_BY", "", "fun"}, {16, "ACCNA_BAR_BI", "", "fun"},
	    {17, "ACCNA_BAR_HI", "", "fun"}, {18, "ACCNA_BAR_MO", "", "fun"}, {19, "ACCNA_BAR_SH", "", "fun"}, {20, "ACCNA_BAR_SI", "", "fun"},
	    {21, "ACCNA_TAXI", "", "jobs"}, {22, "ACCNA_DTRAF", "", "jobs"}, {23, "ACCNA_REPO", "", "jobs"}, {24, "ACCNA_DRFR", "", "bills"},
	    {25, "ACCNA_STRP", "", "fun"}, {26, "ACCNA_HUNT", "", "jobs"}, {27, "ACCNA_RANGE", "", "fun"}, {28, "ACCNA_RACES", "", "jobs"},
	    {29, "ACCNA_EPS_ST", "", "other"}, {30, "ACCNA_EPS_RB", "", "other"}, {31, "ACCNA_SIM", "", "people"}, {32, "ACCNA_LES", "", "people"},
	    {33, "ACCNA_AMA", "", "people"}, {34, "ACCNA_JIM", "", "people"}, {35, "ACCNA_TRA", "", "people"}, {36, "ACCNA_OSC", "", "people"},
	    {37, "ACCNA_ABI", "", "people"}, {38, "ACCNA_BUR", "", "people"}, {39, "ACCNA_MRSPOKE", "", "vehicles"}, {40, "ACCNA_GOL_CLU", "", "fun"},
	    {41, "S_H_01", "", "shopping"}, {42, "S_H_02", "", "shopping"}, {43, "S_H_03", "", "shopping"}, {44, "S_H_04", "", "shopping"},
	    {45, "S_H_05", "", "shopping"}, {46, "S_H_06", "", "shopping"}, {47, "S_H_07", "", "shopping"}, {48, "S_CL_01", "", "shopping"},
	    {49, "S_CL_02", "", "shopping"}, {50, "S_CL_03", "", "shopping"}, {51, "S_CL_04", "", "shopping"}, {52, "S_CL_05", "", "shopping"},
	    {53, "S_CL_06", "", "shopping"}, {54, "S_CL_07", "", "shopping"}, {55, "S_CM_01", "", "shopping"}, {56, "S_CM_03", "", "shopping"},
	    {57, "S_CM_04", "", "shopping"}, {58, "S_CM_05", "", "shopping"}, {59, "S_CH_01", "", "shopping"}, {60, "S_CH_02", "", "shopping"},
	    {61, "S_CH_03", "", "shopping"}, {62, "S_CA_01", "", "shopping"}, {63, "S_T_01", "", "shopping"}, {64, "S_T_02", "", "shopping"},
	    {65, "S_T_03", "", "shopping"}, {66, "S_T_04", "", "shopping"}, {67, "S_T_05", "", "shopping"}, {68, "S_T_06", "", "shopping"},
	    {69, "S_G_01", "", "weapons"}, {70, "S_G_02", "", "weapons"}, {71, "S_G_03", "", "weapons"}, {72, "S_G_04", "", "weapons"},
	    {73, "S_G_05", "", "weapons"}, {74, "S_G_06", "", "weapons"}, {75, "S_G_07", "", "weapons"}, {76, "S_G_08", "", "weapons"},
	    {77, "S_G_09", "", "weapons"}, {78, "S_G_10", "", "weapons"}, {79, "S_G_11", "", "weapons"}, {80, "S_MO_01", "", "vehicles"},
	    {81, "S_MO_05", "", "vehicles"}, {82, "S_MO_06", "", "vehicles"}, {83, "S_MO_07", "", "vehicles"}, {84, "S_MO_08", "", "vehicles"},
	    {85, "ACCNA_CARSITE", "Legendary Motorsport", "vehicles"}, {86, "ACCNA_ARMYSITE", "Warstock Cache & Carry", "vehicles"},
	    {87, "ACCNA_PLANESITE", "Elitás Travel", "vehicles"}, {88, "ACCNA_BOATSITE", "Dock Tease", "vehicles"},
	    {89, "ACCNA_BIKESITE", "Pedal and Metal", "vehicles"}, {90, "ACCNA_AUTOSITE", "Southern San Andreas Super Autos", "vehicles"},
	    {91, "ACCNA_LOSSSITE", "Los Santos Customs", "vehicles"}, {92, "ACCNA_ARENASITE", "Arena War", "vehicles"},
	    {93, "ACCNA_CONSIT", "", "people"}, {94, "ACCNA_TRMSITE", "", "bills"}, {95, "ACCNA_BAILBONDS", "", "jobs"}, {96, "ACCNA_CASHDEP", "", "people"},
	    {97, "ACCNA_HOFFSHORE", "", "jobs"}, {98, "ACCNA_SNACK", "", "shopping"}, {99, "ACCNA_TOWING", "", "vehicles"},
	    {100, "ACCNA_TAXI_LOT", "", "vehicles"}, {101, "ACCNA_ARMS", "", "property"}, {102, "ACCNA_SONAR", "", "property"},
	    {103, "ACCNA_CARMOD", "", "vehicles"}, {104, "ACCNA_VCINEMA", "", "fun"}, {105, "ACCNA_DCINEMA", "", "fun"}, {106, "ACCNA_MCINEMA", "", "fun"},
	    {107, "ACCNA_GOLF", "", "fun"}, {108, "ACCNA_CSCRAP", "", "property"}, {109, "ACCNA_SMOKE", "", "property"}, {110, "ACCNA_TEQUILA", "", "property"},
	    {111, "ACCNA_PITCHERS", "", "property"}, {112, "ACCNA_HEN", "", "property"}, {113, "ACCNA_HOOKIES", "", "property"},
	    {114, "ACCNA_MARINA", "", "property"}, {115, "ACCNA_HANGAR", "", "property"}, {116, "ACCNA_HELIPAD", "", "property"},
	    {117, "ACCNA_GARAGE", "", "property"}, {118, "ACCNA_PD_VB", "", "bills"}, {119, "ACCNA_PD_SC", "", "bills"}, {120, "ACCNA_PD_DT", "", "bills"},
	    {121, "ACCNA_PD_RH", "", "bills"}, {122, "ACCNA_PD_SS", "", "bills"}, {123, "ACCNA_PD_PB", "", "bills"}, {124, "ACCNA_PD_HW", "", "bills"},
	    {125, "ACCNA_H_RH", "", "bills"}, {126, "ACCNA_H_SC", "", "bills"}, {127, "ACCNA_H_DT", "", "bills"}, {128, "ACCNA_H_SS", "", "bills"},
	    {129, "ACCNA_H_PB", "", "bills"}, {130, "ACCNA_DYNPROP", "朝代 8 房地產", "property"}, {131, "S_MO_09", "", "vehicles"},
	};

	const Payee* FindPayee(int id)
	{
		for (const Payee& p : kPayees)
			if (p.id == id)
				return &p;
		return nullptr;
	}

	std::string Text(const char* label)
	{
		if (!label || !*label || !HUD::DOES_TEXT_LABEL_EXIST(label))
			return {};
		std::string s = HUD::GET_FILENAME_FOR_AUDIO_CONVERSATION(label);
		for (size_t at; (at = s.find("\xC2\xB5")) != std::string::npos;) // the game's non-breaking space
			s.replace(at, 2, " ");
		return s;
	}

	std::string PayeeName(int id, int64_t amount)
	{
		if (id < 0)
			return amount >= 0 ? "其他收入" : "其他支出";
		const Payee* p = FindPayee(id);
		if (!p)
			return std::format("帳戶 {}", id);
		std::string name = Text(p->label);
		return name.empty() ? (*p->fallback ? p->fallback : p->label) : name;
	}

	const char* Category(int id)
	{
		const Payee* p = id >= 0 ? FindPayee(id) : nullptr;
		return p ? p->category : "other";
	}

	// ---- game clock ----------------------------------------------------------------------------

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

	// ---- history ---------------------------------------------------------------------------------

	constexpr size_t kMaxEntries = 2000;

	struct Entry
	{
		int64_t t = -1;       // game minute, -1 unknown
		int payee = -1;       // -1: money that moved without a log entry
		int64_t amount = 0;   // + received, - paid
		int64_t balance = -1; // after it; -1 unknown
	};

	struct Account
	{
		bool known = false;  // count / balance below are a baseline
		int count = 0;       // game log entries seen
		int64_t balance = 0; // last balance seen
		std::vector<Entry> entries;
		// A change waits until the log and the balance have both stayed put for a moment (they change together, but
		// a poll can fall between them).
		int pendingCount = -1;
		int64_t pendingBalance = 0;
		uint32_t pendingSince = 0;
	};
	Account g_accounts[3];
	std::atomic_bool g_reload = true;

	ml::game::Character Who(int c) { return static_cast<ml::game::Character>(c); }

	void Save(int c)
	{
		const Account& a = g_accounts[c];
		ml::Json entries = ml::Json::Array();
		for (const Entry& e : a.entries)
			entries.Push(ml::Json::Array({e.t, e.payee, e.amount, e.balance}));
		ml::save::Set("bank", {{"count", a.count}, {"balance", a.balance}, {"entries", entries}}, c);
	}

	void Load()
	{
		for (int c = 0; c < 3; ++c)
		{
			Account& a = g_accounts[c];
			a = {};
			const ml::Json j = ml::save::Get("bank", c);
			if (!j.IsObject())
				continue;
			a.known = true;
			a.count = j["count"].Int();
			a.balance = j["balance"].Int64();
			for (const ml::Json& e : j["entries"].Items())
				a.entries.push_back({e[0].Int64(), e[1].Int(), e[2].Int64(), e[3].Int64()});
		}
	}

	void Add(Account& a, const Entry& e)
	{
		// Untracked money within the same game hour and direction is one entry.
		if (e.payee < 0 && !a.entries.empty())
		{
			Entry& last = a.entries.back();
			if (last.payee < 0 && last.t >= 0 && e.t >= 0 && last.t / 60 == e.t / 60 && (last.amount >= 0) == (e.amount >= 0))
			{
				last.amount += e.amount;
				last.balance = e.balance;
				return;
			}
		}
		a.entries.push_back(e);
		if (a.entries.size() > kMaxEntries)
			a.entries.erase(a.entries.begin(), a.entries.begin() + static_cast<ptrdiff_t>(a.entries.size() - kMaxEntries));
	}

	// The game's newest `n` entries (n <= 11), oldest first.
	std::vector<Entry> GameEntries(int c, int n)
	{
		std::vector<Entry> out;
		const int next = Log(c).Field(1).Int();
		for (int k = n; k >= 1; --k)
		{
			const ml::Global g = GameEntry(c, ((next - k) % kGameSlots + kGameSlots) % kGameSlots);
			const int64_t amount = g.Field(2).Int();
			out.push_back({-1, g.Field(1).Int(), g.Field(0).Int() == 1 ? amount : -amount, -1});
		}
		return out;
	}

	void Track(int c)
	{
		Account& a = g_accounts[c];
		const int count = Log(c).Field(0).Int();
		const int64_t balance = ml::game::Cash(Who(c));
		if (!a.known)
		{
			// First time (no saved history): the game's entries, undated.
			a.known = true;
			a.count = count;
			a.balance = balance;
			for (const Entry& e : GameEntries(c, std::min(count, kGameSlots)))
				a.entries.push_back(e);
			if (!a.entries.empty())
				a.entries.back().balance = balance;
			Save(c);
			return;
		}
		if (count == a.count && balance == a.balance)
		{
			a.pendingCount = -1;
			return;
		}
		const uint32_t now = static_cast<uint32_t>(MISC::GET_GAME_TIMER());
		if (count != a.pendingCount || balance != a.pendingBalance)
		{
			a.pendingCount = count;
			a.pendingBalance = balance;
			a.pendingSince = now;
			return;
		}
		if (now - a.pendingSince < 1000)
			return;
		a.pendingCount = -1;
		const int64_t t = GameMinute();
		std::vector<Entry> logged;
		if (count > a.count)
			logged = GameEntries(c, std::min(count - a.count, kGameSlots));
		// count < a.count: the save was older than the history (loaded without our data); only the balance is new.
		int64_t loggedSum = 0;
		for (const Entry& e : logged)
			loggedSum += e.amount;
		int64_t running = a.balance;
		if (const int64_t other = balance - a.balance - loggedSum; other != 0)
		{
			running += other;
			Add(a, {t, -1, other, running});
		}
		for (Entry e : logged)
		{
			running += e.amount;
			e.t = t;
			e.balance = running;
			Add(a, e);
		}
		a.count = count;
		a.balance = balance;
		Save(c);
	}

	// ---- pages -------------------------------------------------------------------------------------

	constexpr const char* kBanks[3] = {"maze", "fleeca", "liberty"}; // by owner: Michael, Franklin, Trevor

	ml::Json AccountJson(const std::string& bank)
	{
		const int c = ml::game::CharacterIndex();
		int owner = -1;
		for (int i = 0; i < 3; ++i)
			if (bank == kBanks[i])
				owner = i;
		ml::Json out{{"bank", bank}, {"character", c}, {"owner", owner}, {"customer", c >= 0 && c == owner}, {"now", GameMinute()}};
		if (c < 0 || c != owner)
			return out;
		const Account& a = g_accounts[c];
		ml::Json entries = ml::Json::Array();
		for (auto it = a.entries.rbegin(); it != a.entries.rend(); ++it)
			entries.Push({{"t", it->t}, {"payee", it->payee}, {"name", PayeeName(it->payee, it->amount)}, {"category", Category(it->payee)},
			    {"amount", it->amount}, {"balance", it->balance}});
		out["balance"] = ml::game::Cash();
		out["entries"] = entries;
		return out;
	}
}

extern "C" __declspec(dllexport) int MLOnLoad(const MLApi* api, const MLContext* ctx)
{
	ml::Init(api, ctx);
	if (!ml::web::Available() || !ml::save::Available())
	{
		ml::LogError("this loader has no web browser or save data; the bank mod needs them");
		return 0;
	}
	ml::web::Function("bank.account", [](std::string bank) { return AccountJson(bank); });
	ml::game::On(ml::game::Event::SaveLoading, [] { g_reload = true; });
	return 1;
}

extern "C" __declspec(dllexport) void MLMain()
{
	for (;;)
	{
		if (ml::game::CharacterIndex() >= 0 && !ml::game::IsLoadingScreen())
		{
			if (g_reload.exchange(false))
				Load();
			for (int c = 0; c < 3; ++c)
				Track(c);
		}
		ml::Wait(250);
	}
}
