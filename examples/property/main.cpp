// Property mod (work in progress): buy GTA Online apartments and garages in story mode on the Dynasty 8 website of
// the loader's browser (web\www.dynasty8realestate.com; research/phase0.md §28, §29).
//
// Pages call:
//   property.list()   -> { character, cash, properties: [ { id, name, description, price, kind, tier, cars, area,
//                                                            x, y, z, photo, owned } ] }
//   property.buy(id)  -> { ok, error, cash }
// The property data is the game's own (Global 1312440, the Online property table, which story mode fills too); each
// story character owns separately, paying with their own money. Ownership is kept in the mod's save data (ml::save), so
// it follows the game's save like the money paid.
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

	ml::Global EntryGlobal(int id)
	{
		return ml::Global(kPropertyTable).At(id, kEntrySize);
	}
	int64_t* Entry(int id)
	{
		return EntryGlobal(id).Ptr();
	}
	int TierOf(int id)
	{
		return id >= 1 && id <= kLastProperty ? Tier(EntryGlobal(id).Field(31).Int()) : 0;
	}
	int GarageSize(int id)
	{
		const int tier = TierOf(id);
		return tier == 6 || tier == 3 ? 10 : tier == 5 || tier == 2 ? 6 : 2;
	}
	float EntryFloat(int id, int slot)
	{
		return EntryGlobal(id).Field(slot).Float();
	}
	Vector3 EntryPosition(int id, int slot)
	{
		return Vector3(EntryFloat(id, slot), EntryFloat(id, slot + 1), EntryFloat(id, slot + 2));
	}
	std::string Name(int id)
	{
		return Text(EntryGlobal(id).Field(16).Text().c_str());
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
		return EntryGlobal(id).Field(32).Int();
	}
	bool Purchasable(int id)
	{
		return TierOf(id) > 0 && Price(id) > 0;
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

	// ---- ownership: the mod's save data, per character ("owned": [ids]) ------------------------------

	std::set<int> g_owned[3];
	bool g_ownedRead[3] = {};

	const std::set<int>& Owned(int c)
	{
		static const std::set<int> none;
		if (c < 0 || c > 2)
			return none;
		if (!g_ownedRead[c])
		{
			g_ownedRead[c] = true;
			g_owned[c].clear();
			const ml::Json saved = ml::save::Get("owned", c);
			for (const ml::Json& id : saved.Items())
				g_owned[c].insert(id.Int());
			// Before the mod used save data: data\owned_<character>.txt.
			if (saved.IsNull())
				if (std::ifstream in(ml::DataPath(std::format(L"owned_{}.txt", c))); in)
				{
					for (int id; in >> id;)
						g_owned[c].insert(id);
					ml::save::Set("owned", std::vector<int>(g_owned[c].begin(), g_owned[c].end()), c);
				}
		}
		return g_owned[c];
	}
	void AddOwned(int c, int id)
	{
		Owned(c);
		g_owned[c].insert(id);
		ml::save::Set("owned", std::vector<int>(g_owned[c].begin(), g_owned[c].end()), c);
	}

	void NotePurchase()
	{
		// Money is saved with the game and ownership with the mod's save data: an autosave keeps the two together.
		if (ml::game::RequestAutosave())
			ml::Log("purchase: autosave requested");
	}

	// ---- web functions ------------------------------------------------------------------------

	ml::Json List()
	{
		const int c = ml::game::CharacterIndex();
		ml::Json properties = ml::Json::Array();
		for (int id = 1; id <= kLastProperty; ++id)
		{
			if (!Purchasable(id))
				continue;
			const ml::Global e = EntryGlobal(id);
			const int type = e.Field(31).Int(), tier = Tier(type);
			const Vector3 pos = EntryPosition(id, 4);
			properties.Push({{"id", id}, {"name", Name(id)}, {"description", Text(e.Field(20).Text().c_str())}, {"price", Price(id)},
			    {"kind", tier >= 4 ? "apartment" : "garage"}, {"tier", tier}, {"cars", GarageSize(id)},
			    {"area", Text(ZONE::GET_NAME_OF_ZONE(pos.x, pos.y, pos.z))}, {"x", pos.x}, {"y", pos.y}, {"z", pos.z}, {"photo", Photo(type)},
			    {"owned", Owned(c).contains(id)}});
		}
		return {{"character", c}, {"cash", ml::game::Cash()}, {"properties", properties}};
	}

	ml::Json Buy(int id)
	{
		const auto fail = [](const char* error) { return ml::Json{{"ok", false}, {"error", error}, {"cash", ml::game::Cash()}}; };
		if (!Purchasable(id))
			return fail("unknown");
		const int c = ml::game::CharacterIndex();
		if (c < 0)
			return fail("character");
		if (Owned(c).contains(id))
			return fail("owned");
		const int price = Price(id);
		if (!ml::game::AddCash(-price))
			return fail("money");
		AddOwned(c, id);
		garage::Refresh();
		apartment::Refresh();
		NotePurchase();
		ml::Log("character {} bought property {} for ${} (cash now ${})", c, id, price, ml::game::Cash());
		return {{"ok", true}, {"error", ""}, {"cash", ml::game::Cash()}};
	}

	// A save is being loaded (game event, handled in MLMain).
	std::atomic_bool g_saveLoading = false;
}

using namespace property;

extern "C" __declspec(dllexport) int MLOnLoad(const MLApi* api, const MLContext* ctx)
{
	ml::Init(api, ctx);
	if (!ml::web::Available() || !ml::save::Available())
	{
		ml::LogError("this loader has no web browser or save data; the property mod needs them");
		return 0;
	}
	research::OnLoad();
	ml::web::Function("property.list", [] { return List(); });
	ml::web::Function("property.buy", [](int id) { return Buy(id); });
	ml::web::Function("property.cash", [] { return ml::game::Cash(); });
	apartment::RegisterOverrides();
	shop::Start();
	// Loading a save takes back purchases made since it (the loader drops the unsaved save data).
	ml::game::On(ml::game::Event::SaveLoading, [] { g_saveLoading = true; });
	return 1;
}

extern "C" __declspec(dllexport) void MLMain()
{
	for (;;)
	{
		if (g_saveLoading.exchange(false))
		{
			for (bool& read : g_ownedRead)
				read = false;
			garage::Drop();
			apartment::Refresh();
		}
		research::Tick();
		garage::Tick();
		apartment::Tick();
		ml::Wait(0);
	}
}
