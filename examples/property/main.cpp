// Property mod (work in progress): buy GTA Online apartments and garages in story mode through the phone's
// Dynasty 8 website (research/phase0.md §28).
//
// The phone browser (appinternet) runs its Online Dynasty 8 pages: it is told the game is in progress, uses the
// console-style purchase path (prices from the game's property table), sees the story character's cash as its bank
// balance, and keeps its Online property stats in this mod's data folder, one set per story character.
#define NOMINMAX
#include <Windows.h>
#include <ShlObj.h>
#pragma comment(lib, "shell32.lib")
#pragma comment(lib, "ole32.lib")

#include <atomic>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <map>
#include <set>
#include <string>

#include <modloader/natives.hpp>

ML_MOD_INFO("Property", "0.0.2", "ModLoader", "Buy apartments and garages (work in progress)")

namespace
{
	constexpr uint64_t kNetworkIsGameInProgress = 0x10FAB35428CCC9D7ULL;
	constexpr uint64_t kBuyProperty = 0x650A08A280870AF6ULL;
	constexpr uint64_t kUseServerTransactions = 0x7D2708796355B20BULL;
	constexpr uint64_t kBankBalance = 0x76EF28DA05EA395AULL;
	constexpr uint64_t kWalletBalance = 0xA40F9C2623F6A8B5ULL;
	constexpr uint64_t kCanSpendMoney = 0xAB3CAA6B422164DAULL;
	constexpr uint64_t kCanSpendMoney2 = 0x7303E27CC6532080ULL;

	// Story character of the player (0 Michael, 1 Franklin, 2 Trevor), or -1.
	int Character()
	{
		const Hash model = ENTITY::GET_ENTITY_MODEL(PLAYER::PLAYER_PED_ID());
		for (int i = 0; i < 3; ++i)
			if (model == MISC::GET_HASH_KEY(std::format("player_{}", i == 0 ? "zero" : i == 1 ? "one" : "two").c_str()))
				return i;
		return -1;
	}
	Hash CashStat(int character) { return MISC::GET_HASH_KEY(std::format("SP{}_TOTAL_CASH", character).c_str()); }
	// The story character's money: the browser's Online balance.
	int Cash()
	{
		const int c = Character();
		int value = 0;
		if (c >= 0)
			STATS::STAT_GET_INT(CashStat(c), &value, -1);
		return value;
	}
	// Property id (1..131) of an "MP_PROP_<id>" name hash, or 0.
	int PropertyOfHash(Hash hash)
	{
		for (int id = 1; id <= 131; ++id)
			if (MISC::GET_HASH_KEY(std::format("MP_PROP_{}", id).c_str()) == hash)
				return id;
		return 0;
	}

	std::atomic<int> g_buys = 0;

	// Online character stats seen by the browser (property ownership and the like) are kept by this mod, one set per
	// story character, in data\stats_<character>.txt ("hash value" lines). The real Online stats are never touched.
	// Changes follow the game's save: they are written when the game saves (a story save file changes) and dropped
	// when a save is loaded without that (a loading screen), like the money paid for them.
	constexpr uint64_t kStatHashForCharacterStat = 0xD69CE161FE614531ULL;
	constexpr uint64_t kStatGetInt = 0x767FBC2AC802EF3DULL;
	constexpr uint64_t kStatSetInt = 0xB3271D7AB655B441ULL;
	constexpr uint64_t kStatGetBool = 0x11B5E6D2AE73F48EULL;
	constexpr uint64_t kStatSetBool = 0x4B33C4243DE0C432ULL;

	std::set<uint32_t> g_onlineStats;          // hashes the browser built for Online character stats
	std::map<uint32_t, int> g_stats[3];        // per story character, with changes not saved yet
	bool g_statsLoaded[3] = {};
	bool g_statsChanged = false;               // since the last game save

	std::filesystem::path StatsFile(int character)
	{
		return std::filesystem::path(ml::Context().dataDir) / std::format("stats_{}.txt", character);
	}
	std::map<uint32_t, int>* Stats()
	{
		const int c = Character();
		if (c < 0)
			return nullptr;
		if (!g_statsLoaded[c])
		{
			g_statsLoaded[c] = true;
			std::ifstream in(StatsFile(c));
			for (uint32_t hash; in >> std::hex >> hash;)
				if (int value; in >> std::dec >> value)
					g_stats[c][hash] = value;
		}
		return &g_stats[c];
	}
	// The game saved: keep the changes.
	void CommitStats()
	{
		for (int c = 0; c < 3; ++c)
			if (g_statsLoaded[c])
			{
				std::ofstream out(StatsFile(c), std::ios::trunc);
				for (const auto& [hash, value] : g_stats[c])
					out << std::format("{:08X} {}\n", hash, value);
			}
		g_statsChanged = false;
	}
	// A save was loaded: back to what was saved.
	void DropStatChanges()
	{
		for (int c = 0; c < 3; ++c)
		{
			g_stats[c].clear();
			g_statsLoaded[c] = false;
		}
		g_statsChanged = false;
	}

	// Newest change time of the story save files (Documents\Rockstar Games\GTAV Enhanced\Profiles\*\SGTA5*).
	std::filesystem::file_time_type LastGameSave()
	{
		static const std::filesystem::path profiles = [] {
			PWSTR documents = nullptr;
			std::filesystem::path path;
			if (SUCCEEDED(SHGetKnownFolderPath(FOLDERID_Documents, 0, nullptr, &documents)))
				path = std::filesystem::path(documents) / L"Rockstar Games" / L"GTAV Enhanced" / L"Profiles";
			CoTaskMemFree(documents);
			return path;
		}();
		std::filesystem::file_time_type newest{};
		std::error_code ec;
		for (const auto& profile : std::filesystem::directory_iterator(profiles, ec))
			for (const auto& file : std::filesystem::directory_iterator(profile.path(), ec))
				if (file.path().filename().wstring().starts_with(L"SGTA5"))
					newest = std::max(newest, file.last_write_time(ec));
		return newest;
	}
	void OverrideOnlineStats(const char* script)
	{
		ml::scripts::OverrideNative(script, kStatHashForCharacterStat, [](ml::scripts::NativeCall& call) {
			call.CallOriginal();
			g_onlineStats.insert(static_cast<uint32_t>(*call.raw->result));
		});
		const auto get = [](ml::scripts::NativeCall& call) {
			const uint32_t hash = call.Arg<uint32_t>(0);
			auto* stats = g_onlineStats.contains(hash) ? Stats() : nullptr;
			if (!stats)
				return call.CallOriginal();
			const auto it = stats->find(hash);
			if (auto* out = call.Arg<int*>(1))
				*out = it != stats->end() ? it->second : 0;
			call.Return<int64_t>(1);
		};
		const auto set = [](ml::scripts::NativeCall& call) {
			const uint32_t hash = call.Arg<uint32_t>(0);
			auto* stats = g_onlineStats.contains(hash) ? Stats() : nullptr;
			if (!stats)
				return call.CallOriginal();
			if (const int value = call.Arg<int>(1); (*stats)[hash] != value)
			{
				(*stats)[hash] = value;
				g_statsChanged = true;
				ml::Log("online stat {:08X} = {} (character {})", hash, value, Character());
			}
			call.Return<int64_t>(1);
		};
		ml::scripts::OverrideNative(script, kStatGetInt, get);
		ml::scripts::OverrideNative(script, kStatGetBool, get);
		ml::scripts::OverrideNative(script, kStatSetInt, set);
		ml::scripts::OverrideNative(script, kStatSetBool, set);
	}
}

extern "C" __declspec(dllexport) int MLOnLoad(const MLApi* api, const MLContext* ctx)
{
	ml::Init(api, ctx);
	const auto yes = [](ml::scripts::NativeCall& call) { call.Return<int64_t>(1); };
	// Only on Dynasty 8 (website 18): the other sites (the stock markets and so on) stay as in story mode.
	ml::scripts::OverrideNative("appinternet", kNetworkIsGameInProgress, [](ml::scripts::NativeCall& call) {
		if (HUD::GET_CURRENT_WEBSITE_ID() == 18)
			call.Return<int64_t>(1);
		else
			call.CallOriginal();
	});
	// Console-style purchases: prices from the property table instead of the game server catalog.
	ml::scripts::OverrideNative("appinternet", kUseServerTransactions, [](ml::scripts::NativeCall& call) { call.Return<int64_t>(0); });
	// Money: the story character's cash stands in for the Online bank account (wallet empty).
	ml::scripts::OverrideNative("appinternet", kBankBalance, [](ml::scripts::NativeCall& call) { call.Return<int64_t>(Cash()); });
	ml::scripts::OverrideNative("appinternet", kWalletBalance, [](ml::scripts::NativeCall& call) { call.Return<int64_t>(0); });
	const auto canSpend = [](ml::scripts::NativeCall& call) { call.Return<int64_t>(call.Arg<int>(0) <= Cash() ? 1 : 0); };
	ml::scripts::OverrideNative("appinternet", kCanSpendMoney, canSpend);
	ml::scripts::OverrideNative("appinternet", kCanSpendMoney2, canSpend);
	ml::scripts::OverrideNative("appinternet", kBuyProperty, [](ml::scripts::NativeCall& call) {
		++g_buys;
		const int cost = call.Arg<int>(0), c = Character();
		const int id = PropertyOfHash(call.Arg<uint32_t>(1));
		if (c >= 0)
			STATS::STAT_SET_INT(CashStat(c), Cash() - cost, 1);
		ml::Log("NETWORK_BUY_PROPERTY cost {} property {} ({:08X}) character {} -> cash {}", cost, id, call.Arg<uint32_t>(1), c, Cash());
	});
	OverrideOnlineStats("appinternet");
	ml::scripts::OverrideNative("appinternet", 0xB8DFD30D6973E135ULL, yes); // NETWORK_IS_PLAYER_ACTIVE
	return 1;
}

// Story mode's own autosave request (what the game's scripts do, e.g. appinternet @27069): the autosave_controller
// script saves when it can. Returns false when a request is already pending.
bool RequestAutosave()
{
	int64_t* request = ml::scripts::Global(102550);
	if (!request)
		return false;
	if ((request[8] & 0xFFFFFFFF) ? request[10] > 0 : request[10] > 1)
		return false;
	++request[10];
	return true;
}

// Starts the phone's web browser on the Dynasty 8 listing page. Game thread (MLMain).
int OpenBrowser()
{
	// The browser closes itself while the screen is faded out (appinternet @3234013).
	while (CAMERA::IS_SCREEN_FADED_OUT() || CAMERA::IS_SCREEN_FADING_IN())
		ml::Wait(100);
	SCRIPT::REQUEST_SCRIPT("appinternet");
	for (int i = 0; i < 100 && !SCRIPT::HAS_SCRIPT_LOADED("appinternet"); ++i)
		ml::Wait(50);
	// The browser keeps running while this global is set (the phone / computer sets it when opening it).
	if (int64_t* open = ml::scripts::Global(77414))
		*open = 1;
	// Start page: 77528 = 7 takes the page name from Global 77397 (a text label); the Los Santos listing page builds
	// the property list right away (appinternet @5687).
	if (int64_t* start = ml::scripts::Global(77528))
		*start = 7;
	if (auto* url = reinterpret_cast<char*>(ml::scripts::Global(77397)))
		strcpy_s(url, 64, "WWW_DYNASTY8REALESTATE_COM_S_LOS_D_SANTOS");
	const int thread = BUILTIN::START_NEW_SCRIPT("appinternet", 4000);
	SCRIPT::SET_SCRIPT_AS_NO_LONGER_NEEDED("appinternet");
	ml::Log("started appinternet: thread {}", thread);
	return thread;
}

extern "C" __declspec(dllexport) void MLMain()
{
	int saved = 0;   // purchases an autosave was requested for
	int browser = 0; // the browser's thread (appinternet: phone, computer, or open_browser.txt)
	uint64_t nextBrowserCheck = 0;
	bool faked = false;
	int64_t before[3] = {}; // Online state globals the browser runs with, as they were
	auto lastSave = LastGameSave();
	uint64_t nextSaveCheck = 0;
	bool loading = false;
	int lastPage = -1;
	for (;;)
	{
		// Research aid until the phone opens it: ModLoader\open_browser.txt starts the browser.
		if (std::error_code ec; std::filesystem::remove("ModLoader/open_browser.txt", ec))
			browser = OpenBrowser();
		if (browser && !ml::scripts::Static(browser, 0))
			browser = 0;
		if (!browser && ml::Api().GetTickMs() >= nextBrowserCheck)
		{
			nextBrowserCheck = ml::Api().GetTickMs() + 500;
			for (const auto& thread : ml::scripts::Threads())
				if (_stricmp(thread.name.c_str(), "appinternet") == 0) // the phone starts it as "appInternet"
				{
					browser = thread.id;
					ml::Log("browser running: thread {}", browser);
				}
		}
		// On the Dynasty 8 website.
		const bool running = browser && HUD::GET_CURRENT_WEBSITE_ID() == 18;

		// While the browser runs, the Online state its pages check is faked, and put back afterwards: Global 80362
		// (in GTA Online; story scripts such as the autosave controller read it too) and the "local player is in the
		// session" state of the purchase menus (Global 2673276 +2 / +3, appinternet @30268).
		int64_t* online = ml::scripts::Global(80362);
		int64_t* session = ml::scripts::Global(2673276 + 2);
		if (online && session)
		{
			if (running && !faked)
			{
				faked = true;
				before[0] = *online;
				before[1] = session[0];
				before[2] = session[1];
			}
			if (running)
			{
				*online = 1;
				session[0] = 1;
				session[1] = PLAYER::PLAYER_ID();
			}
			else if (faked)
			{
				faked = false;
				*online = before[0];
				session[0] = before[1];
				session[1] = before[2];
			}
		}
		// The Dynasty 8 listing (pages 1 and 2) is built when Global 77588 is set (appinternet @30683); set it once
		// each time the listing is entered.
		if (running)
		{
			const int page = HUD::GET_CURRENT_WEBSITE_ID() == 18 ? HUD::GET_CURRENT_WEBPAGE_ID() : -1;
			// The site's own "browse listings" link leads to its maintenance page (page 25) outside GTA Online (the
			// movie decides that itself); go to the listing page instead, as the browser does for its start page.
			if (page != lastPage)
				ml::Log("Dynasty 8 page {}", page);
			if (page == 25 && page != lastPage)
				if (const int64_t* movie = ml::scripts::Static(browser, 628))
				{
					GRAPHICS::BEGIN_SCALEFORM_MOVIE_METHOD(static_cast<int>(*movie), "GO_TO_WEBPAGE");
					GRAPHICS::BEGIN_TEXT_COMMAND_SCALEFORM_STRING("STRING");
					HUD::ADD_TEXT_COMPONENT_SUBSTRING_WEBSITE("WWW_DYNASTY8REALESTATE_COM_S_LOS_D_SANTOS");
					GRAPHICS::END_TEXT_COMMAND_SCALEFORM_STRING();
					GRAPHICS::END_SCALEFORM_MOVIE_METHOD();
				}
			if (page != lastPage && (page == 1 || page == 2))
				if (int64_t* build = ml::scripts::Global(77588))
					*build = 1;
			lastPage = page;
		}
		else
			lastPage = -1;

		// A purchase changes the character's money (saved with the game) and the property stats (saved by this mod at
		// once): an autosave keeps the two together. Requested once the browser is closed (the autosave controller
		// drops requests while Global 80362 is set).
		if (const int buys = g_buys; buys != saved && !browser && RequestAutosave())
		{
			saved = buys;
			ml::Log("purchase: autosave requested");
		}
		// Property stats follow the game's save (see CommitStats / DropStatChanges).
		if (const bool now = DLC::GET_IS_LOADING_SCREEN_ACTIVE(); now != loading)
		{
			loading = now;
			if (loading && g_statsChanged)
			{
				DropStatChanges();
				ml::Log("save loaded: unsaved property changes dropped");
			}
		}
		if (const uint64_t tick = ml::Api().GetTickMs(); tick >= nextSaveCheck)
		{
			nextSaveCheck = tick + 1000;
			if (const auto save = LastGameSave(); save != lastSave)
			{
				lastSave = save;
				if (g_statsChanged)
				{
					CommitStats();
					ml::Log("game saved: property changes written");
				}
			}
		}
		ml::Wait(browser ? 0 : 200);
	}
}
