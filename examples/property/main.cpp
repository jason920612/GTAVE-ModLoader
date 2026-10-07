// Property mod (work in progress): buy GTA Online apartments and garages in story mode through the phone's
// Dynasty 8 website (research/phase0.md §28).
//
// Stage 1 experiments: the phone browser (appinternet) is told it runs in GTA Online with a working game server
// catalog, to see how far the Online Dynasty 8 pages get in story mode. Price queries are logged.
#include <atomic>
#include <cstring>
#include <filesystem>
#include <map>
#include <mutex>
#include <string>

#include <modloader/natives.hpp>

ML_MOD_INFO("Property", "0.0.2", "ModLoader", "Buy apartments and garages (work in progress)")

namespace
{
	constexpr uint64_t kNetworkIsGameInProgress = 0x10FAB35428CCC9D7ULL;
	constexpr uint64_t kCatalogItemKeyIsValid = 0x247F0F73A182EA0BULL;
	constexpr uint64_t kGetPrice = 0xC27009422FCCA88DULL;
	constexpr uint64_t kSessionValid = 0xB24F0944DA203D9EULL;
	constexpr uint64_t kSessionRefreshPending = 0x810E8431C0614BF9ULL;
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

	std::mutex g_mutex;
	std::map<std::pair<uint32_t, uint32_t>, int> g_prices; // (item, category) -> what the game answered
	std::atomic<int> g_buys = 0;
}

extern "C" __declspec(dllexport) int MLOnLoad(const MLApi* api, const MLContext* ctx)
{
	ml::Init(api, ctx);
	const auto yes = [](ml::scripts::NativeCall& call) { call.Return<int64_t>(1); };
	ml::scripts::OverrideNative("appinternet", kNetworkIsGameInProgress, yes);
	ml::scripts::OverrideNative("appinternet", kCatalogItemKeyIsValid, yes);
	// Console-style purchases: prices from the property table instead of the game server catalog.
	ml::scripts::OverrideNative("appinternet", kUseServerTransactions, [](ml::scripts::NativeCall& call) { call.Return<int64_t>(0); });
	ml::scripts::OverrideNative("appinternet", kSessionValid, yes);
	ml::scripts::OverrideNative("appinternet", kSessionRefreshPending, [](ml::scripts::NativeCall& call) { call.Return<int64_t>(0); });
	ml::scripts::OverrideNative("appinternet", kGetPrice, [](ml::scripts::NativeCall& call) {
		call.CallOriginal();
		std::lock_guard lock(g_mutex);
		g_prices[{call.Arg<uint32_t>(0), call.Arg<uint32_t>(1)}] = call.Arg<int>(0) ? static_cast<int>(*call.raw->result) : 0;
	});
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
	return 1;
}

extern "C" __declspec(dllexport) void MLMain()
{
	size_t reported = 0;
	int lastSite = -1, lastPage = -1;
	int browser = 0;                   // appinternet thread started by open_browser.txt
	std::map<uint32_t, int64_t> watched; // research: browser statics last seen
	for (;;)
	{
		// Research aid: ModLoader\test_scaleform.txt requests the browser's movie and waits for it from the mod.
		if (std::error_code ec; std::filesystem::remove("ModLoader/test_scaleform.txt", ec))
		{
			const int movie = GRAPHICS::REQUEST_SCALEFORM_MOVIE("web_browser");
			int frames = 0;
			for (; frames < 300 && !GRAPHICS::HAS_SCALEFORM_MOVIE_LOADED(movie); ++frames)
				ml::Wait(0);
			ml::Log("scaleform web_browser: handle {}, loaded {} after {} frames", movie, GRAPHICS::HAS_SCALEFORM_MOVIE_LOADED(movie), frames);
		}
		// Research aid: ModLoader\open_browser.txt starts the web browser (like a safehouse computer).
		if (std::error_code ec; std::filesystem::remove("ModLoader/open_browser.txt", ec))
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
			// Start page: 77528 = 7 takes the page name from Global 77397 (a text label); the Los Santos listing page
			// builds the property list right away (appinternet @5687).
			if (int64_t* start = ml::scripts::Global(77528))
				*start = 7;
			if (auto* url = reinterpret_cast<char*>(ml::scripts::Global(77397)))
				strcpy_s(url, 64, "WWW_DYNASTY8REALESTATE_COM_S_LOS_D_SANTOS");
			const int thread = browser = BUILTIN::START_NEW_SCRIPT("appinternet", 4000);
			SCRIPT::SET_SCRIPT_AS_NO_LONGER_NEEDED("appinternet");
			ml::Log("started appinternet: thread {}", thread);
		}
		// Experiment: the listing is only built when these flags (set by GTA Online's scripts) are on.
		const int site = HUD::GET_CURRENT_WEBSITE_ID();
		const int page = HUD::GET_CURRENT_WEBPAGE_ID();
		if (site != lastSite || page != lastPage)
		{
			lastSite = site;
			lastPage = page;
			ml::Log("website {} page {} (globals 80362={}, 77588={})", site, page, *ml::scripts::Global(80362) & 0xFFFFFFFF, *ml::scripts::Global(77588) & 0xFFFFFFFF);
		}
		if (site == 18)
		{
			static bool tableLogged = false;
			if (!tableLogged)
			{
				tableLogged = true;
				const auto g = [](uint32_t i) { const int64_t* v = ml::scripts::Global(i); return v ? static_cast<int>(*v) : -999; };
				ml::Log("property table: size {}, prices {} {} {} {} {}", g(1312440), g(1312440 + 1 + 1 * 1951 + 32), g(1312440 + 1 + 2 * 1951 + 32),
				    g(1312440 + 1 + 3 * 1951 + 32), g(1312440 + 1 + 50 * 1951 + 32), g(1312440 + 1 + 100 * 1951 + 32));
			}
			if (int64_t* g = ml::scripts::Global(80362); g && (*g & 0xFFFFFFFF) == 0)
				*g = 1;
			if (int64_t* g = ml::scripts::Global(77588); g && (*g & 0xFFFFFFFF) == 0)
				*g = 1;
		}
		// Research: the browser's state (website 659, page 661, listing flag 663, ...), logged when it changes.
		for (const uint32_t index : {622u, 628u, 659u, 660u, 661u, 663u, 2013u, 2014u, 2015u, 2017u, 2020u})
			if (const int64_t* v = browser ? ml::scripts::Static(browser, index) : nullptr)
				if (auto it = watched.find(index); it == watched.end() || it->second != *v)
				{
					watched[index] = *v;
					ml::Log("browser static {} = {}", index, *v);
				}
		// Research: the browser's close conditions (appinternet @3234013) and its open flag.
		if (browser)
		{
			const auto g = [](uint32_t i) { const int64_t* v = ml::scripts::Global(i); return v ? static_cast<int>(*v) : -999; };
			const std::string closeState = std::format("1574635+18={} 1575097={} 77413={} 77414={} 2673276+1028={} faded={} cutscene={}", g(1574635 + 18), g(1575097),
			    g(77413), g(77414), g(2673276 + 1023 + 5), static_cast<int>(CAMERA::IS_SCREEN_FADED_OUT()), static_cast<int>(CUTSCENE::IS_CUTSCENE_PLAYING()));
			static std::string lastClose;
			if (closeState != lastClose)
			{
				lastClose = closeState;
				ml::Log("browser close state: {}", closeState);
			}
		}
		{
			std::lock_guard lock(g_mutex);
			if (g_prices.size() != reported)
			{
				for (const auto& [key, price] : g_prices)
					ml::Log("price item {:08X} category {:08X} -> {}", key.first, key.second, price);
				reported = g_prices.size();
			}
		}
		ml::Wait(browser ? 0 : 200);
	}
}
