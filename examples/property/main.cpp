// Property mod (work in progress): buy GTA Online apartments and garages in story mode on the Dynasty 8 website of
// the loader's browser (web\www.dynasty8realestate.com; research/phase0.md §28, §29).
//
// Pages call:
//   property.list()   -> { character, cash, properties: [ { id, name, description, price, kind, tier, cars, area,
//                                                            x, y, z, photo, owned } ] }
//   property.buy(id)  -> { ok, error, cash }
// The property data is the game's own (Global 1312440, the Online property table, which story mode fills too); each
// story character owns separately, paying with their own money. Ownership follows the game's save: it is written when
// the game saves and dropped when a save is loaded without that, like the money paid.
#define NOMINMAX
#include <Windows.h>
#include <ShlObj.h>
#pragma comment(lib, "shell32.lib")
#pragma comment(lib, "ole32.lib")

#include <algorithm>
#include <atomic>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <set>
#include <string>
#include <vector>

#include "property.hpp"

ML_MOD_INFO("Property", "0.1.0", "ModLoader", "Buy apartments and garages on the Dynasty 8 website")

namespace property
{
	// ---- the game's property table ---------------------------------------------------------------

	constexpr uint32_t kPropertyTable = 1312440; // array of 1951-slot entries, index = property id
	constexpr uint32_t kEntrySize = 1951;

	// Category of a property type (appinternet @2376981): 6 / 5 / 4 = high / medium / low end apartment,
	// 3 / 2 / 1 = 10 / 6 / 2 car garage.
	int Tier(int type)
	{
		static const std::vector<std::pair<int, std::vector<int>>> tiers{
		    {6, {1, 2, 3, 4, 5, 6, 7, 34, 35, 36, 37, 38, 39, 40, 41, 42, 43, 61, 62, 63, 64, 65, 73, 74, 75, 76, 77, 78, 79, 80, 81, 82, 83, 84, 85}},
		    {5, {8, 9, 10, 11, 12, 13, 14, 15, 16, 66, 67, 68, 69}},
		    {4, {17, 18, 19, 20, 21, 22, 23, 70, 71, 72}},
		    {3, {24, 26, 27, 54, 56, 57}},
		    {2, {25, 28, 32, 33, 50, 52, 53, 55}},
		    {1, {29, 30, 31, 44, 45, 46, 47, 48, 49, 51, 58, 59, 60}},
		};
		for (const auto& [tier, types] : tiers)
			if (std::find(types.begin(), types.end(), type) != types.end())
				return tier;
		return 0;
	}
	// Photo texture (dictionary and texture of the same name) of a property type (appinternet @3058153).
	std::string Photo(int type)
	{
		static const int numbers[] = {0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15, 16, 17, 18, 19, 20, 21, 22, 23, 24, 25, 26, 27, 28, 29, 30,
		    31, 32, 33, 34, 35, 36, 37, 38, 39, 40, 41, 42, 43, 44, 45, 46, 48, 49, 50, 51, 52, 57, 59, 60, 61, 62, 63, 64, 65, 66, 1, 7, 40, 42, 38, 72,
		    73, 74, 75, 76, 77, 78, 80, 81, 82, 83, 84, 85, 86, 87, 89, 90, 91, 92, 93};
		return type > 0 && type < static_cast<int>(std::size(numbers)) ? std::format("DYN_MP_{}", numbers[type]) : std::string();
	}

	int64_t* Entry(int id)
	{
		return ml::scripts::Global(kPropertyTable + 1 + id * kEntrySize);
	}
	int TierOf(int id)
	{
		const int64_t* e = Entry(id);
		return id >= 1 && id <= kLastProperty && e ? Tier(static_cast<int>(e[31])) : 0;
	}
	int GarageSize(int id)
	{
		const int tier = TierOf(id);
		return tier == 6 || tier == 3 ? 10 : tier == 5 || tier == 2 ? 6 : 2;
	}
	float EntryFloat(int id, int slot)
	{
		const int64_t* e = Entry(id);
		float f = 0;
		if (e)
			std::memcpy(&f, e + slot, 4);
		return f;
	}
	Vector3 EntryPosition(int id, int slot)
	{
		return Vector3(EntryFloat(id, slot), EntryFloat(id, slot + 1), EntryFloat(id, slot + 2));
	}
	std::string Name(int id)
	{
		const int64_t* e = Entry(id);
		return e ? Text(reinterpret_cast<const char*>(e + 16)) : std::string();
	}

	void Help(const std::string& text)
	{
		HUD::BEGIN_TEXT_COMMAND_DISPLAY_HELP("STRING");
		HUD::ADD_TEXT_COMPONENT_SUBSTRING_PLAYER_NAME(text.c_str());
		HUD::END_TEXT_COMMAND_DISPLAY_HELP(0, false, false, -1);
	}
	void Notify(const std::string& text)
	{
		HUD::BEGIN_TEXT_COMMAND_THEFEED_POST("STRING");
		HUD::ADD_TEXT_COMPONENT_SUBSTRING_PLAYER_NAME(text.c_str());
		HUD::END_TEXT_COMMAND_THEFEED_POST_TICKER(false, false);
	}

	int Price(int id)
	{
		const int64_t* e = Entry(id);
		return e ? static_cast<int>(e[32]) : 0;
	}
	bool Purchasable(int id)
	{
		const int64_t* e = Entry(id);
		return id >= 1 && id <= kLastProperty && e && e[32] > 0 && Tier(static_cast<int>(e[31])) > 0;
	}

	// Text of a game label in the game's language ("" when there is none); "µ" (the game's non-breaking space) -> " ".
	std::string Text(const char* label)
	{
		if (!label || !*label || !HUD::DOES_TEXT_LABEL_EXIST(label))
			return {};
		std::string s = HUD::GET_FILENAME_FOR_AUDIO_CONVERSATION(label);
		for (size_t at; (at = s.find("\xC2\xB5")) != std::string::npos;)
			s.replace(at, 2, " ");
		return s;
	}

	std::string Json(const std::string& s)
	{
		std::string out = "\"";
		for (const char c : s)
			switch (c)
			{
			case '"': out += "\\\""; break;
			case '\\': out += "\\\\"; break;
			case '\n': out += "\\n"; break;
			case '\r': break;
			case '\t': out += "\\t"; break;
			default:
				if (static_cast<unsigned char>(c) < 0x20)
					out += std::format("\\u{:04x}", c);
				else
					out += c;
			}
		return out + "\"";
	}

	// ---- the player -------------------------------------------------------------------------

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
	int Cash()
	{
		const int c = Character();
		int value = 0;
		if (c >= 0)
			STATS::STAT_GET_INT(CashStat(c), &value, -1);
		return value;
	}

	// ---- ownership: data\owned_<character>.txt, following the game's save -----------------------

	std::set<int> g_owned[3]; // with changes not saved yet
	bool g_ownedLoaded[3] = {};
	bool g_ownedChanged = false; // since the last game save
	std::atomic<int> g_purchases = 0;
	void NotePurchase() { ++g_purchases; }

	std::filesystem::path OwnedFile(int character)
	{
		return std::filesystem::path(ml::Context().dataDir) / std::format("owned_{}.txt", character);
	}
	std::set<int>& Owned(int c)
	{
		if (!g_ownedLoaded[c])
		{
			g_ownedLoaded[c] = true;
			std::ifstream in(OwnedFile(c));
			for (int id; in >> id;)
				g_owned[c].insert(id);
		}
		return g_owned[c];
	}
	// The game saved: keep the changes.
	void CommitOwned()
	{
		for (int c = 0; c < 3; ++c)
			if (g_ownedLoaded[c])
			{
				std::ofstream out(OwnedFile(c), std::ios::trunc);
				for (const int id : g_owned[c])
					out << id << "\n";
			}
		g_ownedChanged = false;
	}
	// A save was loaded: back to what was saved.
	void DropOwnedChanges()
	{
		for (int c = 0; c < 3; ++c)
		{
			g_owned[c].clear();
			g_ownedLoaded[c] = false;
		}
		g_ownedChanged = false;
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

	// Story mode's own autosave request (what the game's scripts do, e.g. appinternet @27069): the autosave_controller
	// script saves when it can (unless autosave is off in the settings).
	bool RequestAutosave()
	{
		int64_t* request = ml::scripts::Global(102550);
		if (!request || ((request[8] & 0xFFFFFFFF) ? request[10] > 0 : request[10] > 1))
			return false;
		++request[10];
		return true;
	}

	// ---- web functions ------------------------------------------------------------------------

	std::string List()
	{
		const int c = Character();
		std::string out = std::format("{{\"character\":{},\"cash\":{},\"properties\":[", c, Cash());
		bool first = true;
		for (int id = 1; id <= kLastProperty; ++id)
		{
			if (!Purchasable(id))
				continue;
			const int64_t* e = Entry(id);
			const int type = static_cast<int>(e[31]);
			const int tier = Tier(type);
			float pos[3];
			for (int k = 0; k < 3; ++k)
				std::memcpy(&pos[k], e + 4 + k, 4);
			const char* zone = ZONE::GET_NAME_OF_ZONE(pos[0], pos[1], pos[2]);
			const bool owned = c >= 0 && Owned(c).contains(id);
			out += std::format("{}{{\"id\":{},\"name\":{},\"description\":{},\"price\":{},\"kind\":\"{}\",\"tier\":{},\"cars\":{},\"area\":{},"
			                   "\"x\":{:.1f},\"y\":{:.1f},\"z\":{:.1f},\"photo\":{},\"owned\":{}}}",
			    first ? "" : ",", id, Json(Text(reinterpret_cast<const char*>(e + 16))), Json(Text(reinterpret_cast<const char*>(e + 20))), e[32],
			    tier >= 4 ? "apartment" : "garage", tier, tier == 6 || tier == 3 ? 10 : tier == 5 || tier == 2 ? 6 : 2, Json(Text(zone)), pos[0], pos[1],
			    pos[2], Json(Photo(type)), owned ? "true" : "false");
			first = false;
		}
		return out + "]}";
	}

	std::string Buy(const std::string& args)
	{
		const auto fail = [](const char* error) { return std::format("{{\"ok\":false,\"error\":\"{}\",\"cash\":{}}}", error, Cash()); };
		int id = 0;
		if (sscanf_s(args.c_str(), "[%d", &id) != 1 || !Purchasable(id))
			return fail("unknown");
		const int c = Character();
		if (c < 0)
			return fail("character");
		if (Owned(c).contains(id))
			return fail("owned");
		const int price = Price(id), cash = Cash();
		if (cash < price)
			return fail("money");
		STATS::STAT_SET_INT(CashStat(c), cash - price, 1);
		Owned(c).insert(id);
		g_ownedChanged = true;
		garage::Refresh();
		apartment::Refresh();
		++g_purchases;
		ml::Log("character {} bought property {} for ${} (cash ${} -> ${})", c, id, price, cash, Cash());
		return std::format("{{\"ok\":true,\"error\":\"\",\"cash\":{}}}", Cash());
	}
}

using namespace property;

extern "C" __declspec(dllexport) int MLOnLoad(const MLApi* api, const MLContext* ctx)
{
	ml::Init(api, ctx);
	if (!ml::web::Available())
	{
		ml::LogError("this loader has no web browser; the property mod needs it");
		return 0;
	}
	research::OnLoad();
	ml::web::Function("property.list", [](const std::string&) { return List(); });
	ml::web::Function("property.buy", [](const std::string& args) { return Buy(args); });
	apartment::RegisterOverrides();
	shop::RegisterWebFunctions();
	ml::web::Function("property.cash", [](const std::string&) { return std::to_string(Cash()); });
	return 1;
}

extern "C" __declspec(dllexport) void MLMain()
{
	int saved = 0; // purchases an autosave was requested for
	auto lastSave = LastGameSave();
	uint64_t nextSaveCheck = 0;
	bool loading = false;
	for (;;)
	{
		// A purchase changes the character's money (saved with the game) and the ownership (written when the game
		// saves): an autosave keeps the two together.
		if (const int purchases = g_purchases; purchases != saved && RequestAutosave())
		{
			saved = purchases;
			ml::Log("purchase: autosave requested");
		}

		research::Tick();
		if (const bool now = DLC::GET_IS_LOADING_SCREEN_ACTIVE(); now != loading)
		{
			loading = now;
			if (loading && g_ownedChanged)
			{
				DropOwnedChanges();
				garage::Refresh();
				apartment::Refresh();
				ml::Log("save loaded: unsaved purchases dropped");
			}
			if (loading && garage::Changed())
			{
				garage::Drop();
				ml::Log("save loaded: unsaved garage changes dropped");
			}
		}
		if (const uint64_t tick = ml::Api().GetTickMs(); tick >= nextSaveCheck)
		{
			nextSaveCheck = tick + 1000;
			if (const auto save = LastGameSave(); save != lastSave)
			{
				lastSave = save;
				if (g_ownedChanged)
				{
					CommitOwned();
					ml::Log("game saved: purchases written");
				}
				if (garage::Changed())
				{
					garage::Commit();
					ml::Log("game saved: garages written");
				}
			}
		}
		garage::Tick();
		apartment::Tick();
		shop::Tick();
		ml::Wait(0);
	}
}
