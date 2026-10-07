// Apartments of the properties the player owns: the GTA Online apartment interiors (story mode has them), a door
// marker outside, and inside: the front door (leave, or take the elevator to the garage), and the bedroom (sleep and
// save with the game's save menu, wardrobe of saved outfits). Positions come from the property table
// (research/phase0.md §32).
#define NOMINMAX
#include <filesystem>
#include <fstream>
#include <map>
#include <sstream>
#include <vector>

#include "property.hpp"

namespace property::apartment
{
	namespace
	{
		// ---- positions in the property table ---------------------------------------------------------

		Place At(int id, int slot)
		{
			const Vector3 v = EntryPosition(id, slot);
			return {v.x, v.y, v.z, EntryFloat(id, slot + 3)};
		}
		bool Set(const Place& p)
		{
			return p.x != 0 || p.y != 0;
		}
		Place Door(int id) { return At(id, 0); }         // the building's entrance
		Place Outside(int id) { return At(id, 51); }     // where the player comes back out
		Place Arrive(int id) { return At(id, 47); }      // inside, by the front door
		Place FrontDoor(int id) { return At(id, 1663); } // inside: the front door
		Place Bedroom(int id) { return At(id, 331); }    // inside: between the bed and the wardrobe

		bool IsApartment(int id)
		{
			return TierOf(id) >= 4;
		}

		// ---- outfits: data\outfits_<character>.txt (a wardrobe, not game progress: written at once) -------

		struct Outfit
		{
			std::string name;
			int drawable[12]{}, texture[12]{}, palette[12]{};
			int prop[8]{}, propTexture[8]{};
		};
		std::vector<Outfit> g_outfits[3];
		bool g_outfitsLoaded[3] = {};

		std::filesystem::path OutfitFile(int c)
		{
			return std::filesystem::path(ml::Context().dataDir) / std::format("outfits_{}.txt", c);
		}
		std::vector<Outfit>& Outfits(int c)
		{
			if (!g_outfitsLoaded[c])
			{
				g_outfitsLoaded[c] = true;
				std::ifstream in(OutfitFile(c));
				for (std::string name, numbers; std::getline(in, name) && std::getline(in, numbers);)
				{
					Outfit o;
					o.name = name;
					std::istringstream n(numbers);
					for (int i = 0; i < 12; ++i)
						n >> o.drawable[i] >> o.texture[i] >> o.palette[i];
					for (int i = 0; i < 8; ++i)
						n >> o.prop[i] >> o.propTexture[i];
					if (n)
						g_outfits[c].push_back(o);
				}
			}
			return g_outfits[c];
		}
		void SaveOutfits(int c)
		{
			std::ofstream out(OutfitFile(c), std::ios::trunc);
			for (const Outfit& o : g_outfits[c])
			{
				out << o.name << '\n';
				for (int i = 0; i < 12; ++i)
					out << o.drawable[i] << ' ' << o.texture[i] << ' ' << o.palette[i] << ' ';
				for (int i = 0; i < 8; ++i)
					out << o.prop[i] << ' ' << o.propTexture[i] << ' ';
				out << '\n';
			}
		}
		Outfit Current(const std::string& name)
		{
			const Ped ped = PLAYER::PLAYER_PED_ID();
			Outfit o;
			o.name = name;
			for (int i = 0; i < 12; ++i)
			{
				o.drawable[i] = PED::GET_PED_DRAWABLE_VARIATION(ped, i);
				o.texture[i] = PED::GET_PED_TEXTURE_VARIATION(ped, i);
				o.palette[i] = PED::GET_PED_PALETTE_VARIATION(ped, i);
			}
			for (int i = 0; i < 8; ++i)
			{
				o.prop[i] = PED::GET_PED_PROP_INDEX(ped, i, 0);
				o.propTexture[i] = PED::GET_PED_PROP_TEXTURE_INDEX(ped, i);
			}
			return o;
		}
		void Wear(const Outfit& o)
		{
			const Ped ped = PLAYER::PLAYER_PED_ID();
			for (int i = 0; i < 12; ++i)
				PED::SET_PED_COMPONENT_VARIATION(ped, i, o.drawable[i], o.texture[i], o.palette[i]);
			for (int i = 0; i < 8; ++i)
				if (o.prop[i] >= 0)
					PED::SET_PED_PROP_INDEX(ped, i, o.prop[i], o.propTexture[i], true, 0);
				else
					PED::CLEAR_PED_PROP(ped, i, 0);
		}

		std::string Json(const std::string& s)
		{
			std::string out = "\"";
			for (const char ch : s)
				if (ch == '"' || ch == '\\')
					(out += '\\') += ch;
				else if (static_cast<unsigned char>(ch) >= 0x20)
					out += ch;
			return out + "\"";
		}
		// The first JSON string argument of a page call ("" when there is none).
		std::string StringArg(const std::string& args)
		{
			const size_t a = args.find('"');
			if (a == std::string::npos)
				return {};
			std::string s;
			for (size_t i = a + 1; i < args.size() && args[i] != '"'; ++i)
				s += args[i] == '\\' && i + 1 < args.size() ? args[++i] : args[i];
			return s;
		}
		int IntArg(const std::string& args)
		{
			int v = -1;
			sscanf_s(args.c_str(), "[%d", &v);
			return v;
		}

		// ---- state ---------------------------------------------------------------------------

		int g_inside = 0; // apartment the player is in
		int g_character = -1;
		bool g_refresh = true;
		int g_justLeft = 0;
		std::map<int, Blip> g_blips;

		void RebuildBlips(int c)
		{
			for (auto& [id, blip] : g_blips)
				HUD::REMOVE_BLIP(&blip);
			g_blips.clear();
			if (c < 0)
				return;
			for (const int id : Owned(c))
			{
				const Place door = Door(id);
				if (!IsApartment(id) || !Set(door))
					continue;
				const Blip blip = HUD::ADD_BLIP_FOR_COORD(door.x, door.y, door.z);
				HUD::SET_BLIP_SPRITE(blip, 40); // safehouse
				HUD::SET_BLIP_AS_SHORT_RANGE(blip, true);
				HUD::BEGIN_TEXT_COMMAND_SET_BLIP_NAME("STRING");
				HUD::ADD_TEXT_COMPONENT_SUBSTRING_PLAYER_NAME(Name(id).c_str());
				HUD::END_TEXT_COMMAND_SET_BLIP_NAME(blip);
				g_blips[id] = blip;
			}
		}

		void LeaveTo(const Place& to)
		{
			Fade(true);
			LoadAt(to.x, to.y, to.z);
			MovePlayer(to);
			g_justLeft = g_inside;
			g_inside = 0;
			RestoreInterior();
			ml::Wait(300);
			Fade(false);
		}

		// Sleep until morning-ish (six hours), then the game's own save menu.
		void Sleep()
		{
			const Ped ped = PLAYER::PLAYER_PED_ID();
			Fade(true);
			CLOCK::ADD_TO_CLOCK_TIME(6, 0, 0);
			ENTITY::SET_ENTITY_HEALTH(ped, ENTITY::GET_ENTITY_MAX_HEALTH(ped), 0, 0);
			ml::Wait(1500);
			Fade(false);
			ml::Wait(500);
			MISC::SET_SAVE_MENU_ACTIVE(false);
		}
	}

	void Enter(int id)
	{
		const Place arrive = Arrive(id);
		if (!Set(arrive))
			return;
		Fade(true);
		LoadAt(arrive.x, arrive.y, arrive.z);
		MovePlayer(arrive);
		g_inside = id;
		ml::Wait(300);
		Fade(false);
	}

	bool Inside()
	{
		return g_inside != 0;
	}

	void Refresh()
	{
		g_refresh = true;
	}

	void RegisterWebFunctions()
	{
		ml::web::Function("wardrobe.list", [](const std::string&) {
			const int c = Character();
			std::string out = std::format("{{\"character\":{},\"outfits\":[", c);
			if (c >= 0)
			{
				bool first = true;
				for (const Outfit& o : Outfits(c))
				{
					out += (first ? "" : ",") + Json(o.name);
					first = false;
				}
			}
			return out + "]}";
		});
		ml::web::Function("wardrobe.save", [](const std::string& args) {
			const int c = Character();
			if (c < 0)
				return std::string("false");
			std::string name = StringArg(args);
			if (name.empty())
				name = std::format("服裝 {}", Outfits(c).size() + 1);
			Outfits(c).push_back(Current(name));
			SaveOutfits(c);
			return std::string("true");
		});
		ml::web::Function("wardrobe.wear", [](const std::string& args) {
			const int c = Character(), i = IntArg(args);
			if (c < 0 || i < 0 || i >= static_cast<int>(Outfits(c).size()))
				return std::string("false");
			Wear(Outfits(c)[i]);
			return std::string("true");
		});
		ml::web::Function("wardrobe.remove", [](const std::string& args) {
			const int c = Character(), i = IntArg(args);
			if (c < 0 || i < 0 || i >= static_cast<int>(Outfits(c).size()))
				return std::string("false");
			Outfits(c).erase(Outfits(c).begin() + i);
			SaveOutfits(c);
			return std::string("true");
		});
	}

	void Tick()
	{
		const int c = Character();
		if (c != g_character || g_refresh)
		{
			if (c != g_character)
				g_inside = 0;
			g_character = c;
			g_refresh = false;
			RebuildBlips(c);
		}
		if (c < 0 || garage::Inside())
			return;
		const Player player = PLAYER::PLAYER_ID();
		const Ped ped = PLAYER::PLAYER_PED_ID();
		if (PED::IS_PED_DEAD_OR_DYING(ped, true) || !PLAYER::IS_PLAYER_CONTROL_ON(player) || STREAMING::IS_PLAYER_SWITCH_IN_PROGRESS() ||
		    !PED::IS_PED_ON_FOOT(ped))
			return;
		const Vector3 at = ENTITY::GET_ENTITY_COORDS(ped, true);

		if (g_inside)
		{
			const int id = g_inside;
			const Place arrive = Arrive(id);
			if (!Near(at, arrive.x, arrive.y, arrive.z, 60.0f))
			{
				g_inside = 0; // left some other way
				RestoreInterior();
				return;
			}
			const Place door = FrontDoor(id), bed = Bedroom(id);
			Marker(door.x, door.y, door.z, 1.0f);
			Marker(bed.x, bed.y, bed.z, 1.0f);
			if (Near(at, door.x, door.y, door.z, 1.2f))
			{
				Help("按 ~INPUT_CONTEXT~ 出門\n按 ~INPUT_CONTEXT_SECONDARY~ 搭電梯到車庫");
				if (PAD::IS_CONTROL_JUST_PRESSED(0, 51))
					LeaveTo(Outside(id));
				else if (PAD::IS_CONTROL_JUST_PRESSED(0, 52))
				{
					ml::Log("apartment {}: elevator to the garage", id);
					g_inside = 0;
					garage::EnterOnFoot(id);
				}
			}
			else if (Near(at, bed.x, bed.y, bed.z, 1.2f))
			{
				Help("按 ~INPUT_CONTEXT~ 睡覺並存檔\n按 ~INPUT_CONTEXT_SECONDARY~ 打開衣櫃");
				if (PAD::IS_CONTROL_JUST_PRESSED(0, 51))
					Sleep();
				else if (PAD::IS_CONTROL_JUST_PRESSED(0, 52))
					ml::web::Open("https://wardrobe.dynasty8/");
			}
			return;
		}

		for (const int id : Owned(c))
		{
			if (!IsApartment(id))
				continue;
			const Place door = Door(id);
			if (!Set(door) || !Near(at, door.x, door.y, door.z, 60.0f))
				continue;
			Marker(door.x, door.y, door.z);
			if (!Near(at, door.x, door.y, door.z, 1.5f))
			{
				if (g_justLeft == id && !Near(at, door.x, door.y, door.z, 6.0f))
					g_justLeft = 0;
				continue;
			}
			if (g_justLeft == id)
				continue;
			if (PLAYER::GET_PLAYER_WANTED_LEVEL(player) > 0)
			{
				Help("被通緝時無法進入公寓。");
				continue;
			}
			Help(std::format("{}\n按 ~INPUT_CONTEXT~ 進入公寓", Name(id)));
			if (PAD::IS_CONTROL_JUST_PRESSED(0, 51))
				Enter(id);
			break;
		}
	}
}
