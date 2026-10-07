// Garages of the properties the player owns: a marker at the garage entrance (on foot or driving), the GTA Online
// garage interiors (story mode has them), vehicles stored with their modifications, one set per story character.
// Stored vehicles follow the game's save like the ownership (data\garage_<character>.txt).
#define NOMINMAX
#include <algorithm>
#include <array>
#include <cmath>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <map>
#include <sstream>
#include <vector>

#include "property.hpp"

namespace property::garage
{
	namespace
	{
		// ---- the three Online garage interiors (startup.ysc @46170, filled per garage size) ------------

		struct Layout
		{
			Place arrive;                  // where the player appears
			Place exit;                    // the door to walk out of
			std::vector<Place> slots;      // parking spaces
		};
		const Layout& LayoutFor(int size)
		{
			static const Layout two{{173.14f, -1008.10f, -100.0f, 343.28f},
			    {178.46f, -1006.16f, -100.0f, 98.85f},
			    {{171.47f, -1003.68f, -100.0f, 178.41f}, {175.20f, -1003.82f, -100.0f, 178.41f}}};
			static const Layout six{{206.19f, -1006.42f, -100.0f, 11.22f},
			    {206.76f, -999.16f, -100.0f, 84.81f},
			    {{193.16f, -1003.33f, -100.0f, 0.02f}, {196.69f, -1003.33f, -100.0f, 0.02f}, {200.19f, -1003.33f, -100.0f, 0.02f},
			        {203.81f, -1003.33f, -100.0f, 0.02f}, {193.54f, -997.60f, -100.0f, 211.03f}, {198.03f, -997.22f, -100.0f, 206.57f}}};
			static const Layout ten{{229.22f, -1005.10f, -100.0f, 352.77f},
			    {237.60f, -1004.75f, -100.0f, 80.28f},
			    {{224.34f, -980.85f, -100.0f, 241.4f}, {224.34f, -986.35f, -100.0f, 241.4f}, {224.34f, -991.85f, -100.0f, 241.4f},
			        {224.34f, -997.35f, -100.0f, 241.4f}, {224.34f, -1002.85f, -100.0f, 241.4f}, {232.65f, -980.85f, -100.0f, 116.31f},
			        {232.65f, -986.35f, -100.0f, 116.31f}, {232.65f, -991.85f, -100.0f, 116.31f}, {232.65f, -997.35f, -100.0f, 116.31f},
			        {232.65f, -1002.85f, -100.0f, 116.31f}}};
			return size >= 10 ? ten : size >= 6 ? six : two;
		}

		// ---- the outside: entrance and where the player comes back out ------------------------------

		bool Set(const Vector3& v)
		{
			return v.x != 0 || v.y != 0;
		}
		// Apartments: the building's garage entrance (slot 7); garages: their door (slot 0).
		Vector3 Entrance(int id)
		{
			if (TierOf(id) >= 4)
				if (const Vector3 v = EntryPosition(id, 7); Set(v))
					return v;
			return EntryPosition(id, 0);
		}
		// On foot: next to the entrance (slot 78 for apartments' garages, else 51), facing away from it.
		Place OutsideOnFoot(int id)
		{
			for (const int slot : {TierOf(id) >= 4 ? 78 : 51, 51})
				if (const Vector3 v = EntryPosition(id, slot); Set(v))
					return {v.x, v.y, v.z, EntryFloat(id, slot + 3)};
			const Vector3 v = Entrance(id);
			return {v.x, v.y, v.z, 0};
		}

		// ---- a stored vehicle ------------------------------------------------------------------

		struct Stored
		{
			Hash model = 0;
			int primary = 0, secondary = 0, pearl = 0, wheel = 0, interior = 0, dashboard = 0;
			int customPrimary = -1, customSecondary = -1; // 0xRRGGBB, -1 = none
			int modKit = 0, wheelType = 0;
			std::array<int, 50> mods{};
			int frontVariation = 0, rearVariation = 0;
			int turbo = 0, xenon = 0, tyreSmokeOn = 0;
			int smoke = 0, neon = 0, neonOn = 0; // 0xRRGGBB, bits 0..3
			int plateIndex = 0, tint = 0, livery = -1, extras = 0, bulletproof = 0;
			std::string plate;
		};

		int Rgb(int r, int g, int b)
		{
			return (r & 255) << 16 | (g & 255) << 8 | (b & 255);
		}

		Stored Capture(Vehicle v)
		{
			Stored s;
			s.model = ENTITY::GET_ENTITY_MODEL(v);
			VEHICLE::GET_VEHICLE_COLOURS(v, &s.primary, &s.secondary);
			VEHICLE::GET_VEHICLE_EXTRA_COLOURS(v, &s.pearl, &s.wheel);
			VEHICLE::GET_VEHICLE_EXTRA_COLOUR_5(v, &s.interior);
			VEHICLE::GET_VEHICLE_EXTRA_COLOUR_6(v, &s.dashboard);
			int r, g, b;
			if (VEHICLE::GET_IS_VEHICLE_PRIMARY_COLOUR_CUSTOM(v))
			{
				VEHICLE::GET_VEHICLE_CUSTOM_PRIMARY_COLOUR(v, &r, &g, &b);
				s.customPrimary = Rgb(r, g, b);
			}
			if (VEHICLE::GET_IS_VEHICLE_SECONDARY_COLOUR_CUSTOM(v))
			{
				VEHICLE::GET_VEHICLE_CUSTOM_SECONDARY_COLOUR(v, &r, &g, &b);
				s.customSecondary = Rgb(r, g, b);
			}
			s.wheelType = VEHICLE::GET_VEHICLE_WHEEL_TYPE(v);
			for (int i = 0; i < 50; ++i)
				s.mods[i] = VEHICLE::GET_VEHICLE_MOD(v, i);
			s.frontVariation = VEHICLE::GET_VEHICLE_MOD_VARIATION(v, 23);
			s.rearVariation = VEHICLE::GET_VEHICLE_MOD_VARIATION(v, 24);
			s.turbo = VEHICLE::IS_TOGGLE_MOD_ON(v, 18);
			s.tyreSmokeOn = VEHICLE::IS_TOGGLE_MOD_ON(v, 20);
			s.xenon = VEHICLE::IS_TOGGLE_MOD_ON(v, 22);
			VEHICLE::GET_VEHICLE_TYRE_SMOKE_COLOR(v, &r, &g, &b);
			s.smoke = Rgb(r, g, b);
			VEHICLE::GET_VEHICLE_NEON_COLOUR(v, &r, &g, &b);
			s.neon = Rgb(r, g, b);
			for (int i = 0; i < 4; ++i)
				if (VEHICLE::GET_VEHICLE_NEON_ENABLED(v, i))
					s.neonOn |= 1 << i;
			s.plateIndex = VEHICLE::GET_VEHICLE_NUMBER_PLATE_TEXT_INDEX(v);
			if (const char* plate = VEHICLE::GET_VEHICLE_NUMBER_PLATE_TEXT(v))
				s.plate = plate;
			s.tint = VEHICLE::GET_VEHICLE_WINDOW_TINT(v);
			s.livery = VEHICLE::GET_VEHICLE_LIVERY(v);
			for (int i = 1; i <= 14; ++i)
				if (VEHICLE::DOES_EXTRA_EXIST(v, i) && VEHICLE::IS_VEHICLE_EXTRA_TURNED_ON(v, i))
					s.extras |= 1 << i;
			s.bulletproof = !VEHICLE::GET_VEHICLE_TYRES_CAN_BURST(v);
			return s;
		}

		void Apply(Vehicle v, const Stored& s)
		{
			VEHICLE::SET_VEHICLE_MOD_KIT(v, 0);
			VEHICLE::SET_VEHICLE_WHEEL_TYPE(v, s.wheelType);
			for (int i = 0; i < 50; ++i)
				if (s.mods[i] >= 0)
					VEHICLE::SET_VEHICLE_MOD(v, i, s.mods[i], i == 23 ? s.frontVariation : i == 24 ? s.rearVariation : false);
			VEHICLE::TOGGLE_VEHICLE_MOD(v, 18, s.turbo);
			VEHICLE::TOGGLE_VEHICLE_MOD(v, 20, s.tyreSmokeOn);
			VEHICLE::TOGGLE_VEHICLE_MOD(v, 22, s.xenon);
			VEHICLE::SET_VEHICLE_COLOURS(v, s.primary, s.secondary);
			VEHICLE::SET_VEHICLE_EXTRA_COLOURS(v, s.pearl, s.wheel);
			VEHICLE::SET_VEHICLE_EXTRA_COLOUR_5(v, s.interior);
			VEHICLE::SET_VEHICLE_EXTRA_COLOUR_6(v, s.dashboard);
			if (s.customPrimary >= 0)
				VEHICLE::SET_VEHICLE_CUSTOM_PRIMARY_COLOUR(v, s.customPrimary >> 16 & 255, s.customPrimary >> 8 & 255, s.customPrimary & 255);
			if (s.customSecondary >= 0)
				VEHICLE::SET_VEHICLE_CUSTOM_SECONDARY_COLOUR(v, s.customSecondary >> 16 & 255, s.customSecondary >> 8 & 255, s.customSecondary & 255);
			VEHICLE::SET_VEHICLE_TYRE_SMOKE_COLOR(v, s.smoke >> 16 & 255, s.smoke >> 8 & 255, s.smoke & 255);
			VEHICLE::SET_VEHICLE_NEON_COLOUR(v, s.neon >> 16 & 255, s.neon >> 8 & 255, s.neon & 255);
			for (int i = 0; i < 4; ++i)
				VEHICLE::SET_VEHICLE_NEON_ENABLED(v, i, (s.neonOn >> i) & 1);
			VEHICLE::SET_VEHICLE_NUMBER_PLATE_TEXT_INDEX(v, s.plateIndex);
			if (!s.plate.empty())
				VEHICLE::SET_VEHICLE_NUMBER_PLATE_TEXT(v, s.plate.c_str());
			VEHICLE::SET_VEHICLE_WINDOW_TINT(v, s.tint);
			if (s.livery >= 0)
				VEHICLE::SET_VEHICLE_LIVERY(v, s.livery);
			for (int i = 1; i <= 14; ++i)
				if (VEHICLE::DOES_EXTRA_EXIST(v, i))
					VEHICLE::SET_VEHICLE_EXTRA(v, i, !((s.extras >> i) & 1));
			VEHICLE::SET_VEHICLE_TYRES_CAN_BURST(v, !s.bulletproof);
			VEHICLE::SET_VEHICLE_DIRT_LEVEL(v, 0.0f);
		}

		// One line per vehicle: "<property> <slot> <model> <numbers...> <plate>" (plate spaces as '_').
		std::string Serialize(int id, int slot, const Stored& s)
		{
			std::ostringstream o;
			o << id << ' ' << slot << ' ' << s.model << ' ' << s.primary << ' ' << s.secondary << ' ' << s.pearl << ' ' << s.wheel << ' ' << s.interior
			  << ' ' << s.dashboard << ' ' << s.customPrimary << ' ' << s.customSecondary << ' ' << s.modKit << ' ' << s.wheelType;
			for (const int m : s.mods)
				o << ' ' << m;
			o << ' ' << s.frontVariation << ' ' << s.rearVariation << ' ' << s.turbo << ' ' << s.xenon << ' ' << s.tyreSmokeOn << ' ' << s.smoke << ' '
			  << s.neon << ' ' << s.neonOn << ' ' << s.plateIndex << ' ' << s.tint << ' ' << s.livery << ' ' << s.extras << ' ' << s.bulletproof << ' ';
			std::string plate = s.plate.empty() ? "-" : s.plate;
			std::replace(plate.begin(), plate.end(), ' ', '_');
			o << plate;
			return o.str();
		}
		bool Parse(const std::string& line, int& id, int& slot, Stored& s)
		{
			std::istringstream in(line);
			in >> id >> slot >> s.model >> s.primary >> s.secondary >> s.pearl >> s.wheel >> s.interior >> s.dashboard >> s.customPrimary >>
			    s.customSecondary >> s.modKit >> s.wheelType;
			for (int& m : s.mods)
				in >> m;
			in >> s.frontVariation >> s.rearVariation >> s.turbo >> s.xenon >> s.tyreSmokeOn >> s.smoke >> s.neon >> s.neonOn >> s.plateIndex >> s.tint >>
			    s.livery >> s.extras >> s.bulletproof >> s.plate;
			if (s.plate == "-")
				s.plate.clear();
			std::replace(s.plate.begin(), s.plate.end(), '_', ' ');
			return !in.fail();
		}

		// ---- storage per character: property -> slot -> vehicle ----------------------------------

		using Garage = std::map<int, Stored>;
		std::map<int, Garage> g_stored[3];
		bool g_loaded[3] = {};
		bool g_changed = false;

		std::filesystem::path File(int c)
		{
			return std::filesystem::path(ml::Context().dataDir) / std::format("garage_{}.txt", c);
		}
		std::map<int, Garage>& Stores(int c)
		{
			if (!g_loaded[c])
			{
				g_loaded[c] = true;
				std::ifstream in(File(c));
				for (std::string line; std::getline(in, line);)
				{
					int id = 0, slot = 0;
					Stored s;
					if (Parse(line, id, slot, s))
						g_stored[c][id][slot] = s;
				}
			}
			return g_stored[c];
		}

		// ---- in the garage ----------------------------------------------------------------------

		int g_inside = 0; // property whose garage the player is in, 0 = outside
		int g_character = -1;
		std::map<int, Vehicle> g_spawned; // slot -> vehicle shown in the garage
		std::map<int, Blip> g_blips;
		bool g_refresh = true;
		int g_justLeft = 0; // no prompt at this entrance until the player has moved off it

		Vehicle SpawnVehicle(const Stored& s, const Place& at)
		{
			STREAMING::REQUEST_MODEL(s.model);
			for (int i = 0; i < 200 && !STREAMING::HAS_MODEL_LOADED(s.model); ++i)
				ml::Wait(10);
			if (!STREAMING::HAS_MODEL_LOADED(s.model))
				return 0;
			// Placed on the garage floor as it is: the ground probe would find the street above the (underground) garage.
			const Vehicle v = VEHICLE::CREATE_VEHICLE(s.model, at.x, at.y, at.z + 0.3f, at.heading, false, false, false);
			STREAMING::SET_MODEL_AS_NO_LONGER_NEEDED(s.model);
			if (!v)
				return 0;
			Apply(v, s);
			VEHICLE::SET_VEHICLE_HAS_BEEN_OWNED_BY_PLAYER(v, true);
			return v;
		}

		void ClearSpawned(int keep = 0)
		{
			for (auto& [slot, v] : g_spawned)
				if (v && v != keep && ENTITY::DOES_ENTITY_EXIST(v))
					VEHICLE::DELETE_VEHICLE(&v);
			g_spawned.clear();
		}

		// Into the garage; `drive` = the vehicle the player drives in with (stored first), or 0.
		void Enter(int id, Vehicle drive)
		{
			const int c = Character();
			Garage& garage = Stores(c)[id];
			const Layout& layout = LayoutFor(GarageSize(id));
			int freeSlot = -1;
			for (int i = 0; i < static_cast<int>(layout.slots.size()) && freeSlot < 0; ++i)
				if (!garage.contains(i))
					freeSlot = i;
			if (drive && freeSlot < 0)
			{
				Notify("車庫已滿，無法停入這輛車。");
				return;
			}
			Fade(true);
			const Ped ped = PLAYER::PLAYER_PED_ID();
			if (drive)
			{
				garage[freeSlot] = Capture(drive);
				g_changed = true;
				ENTITY::SET_ENTITY_COORDS(ped, layout.arrive.x, layout.arrive.y, layout.arrive.z, false, false, false, false);
				ENTITY::SET_ENTITY_AS_MISSION_ENTITY(drive, true, true);
				VEHICLE::DELETE_VEHICLE(&drive);
				ml::Log("garage {}: stored a vehicle in slot {}", id, freeSlot);
			}
			LoadAt(layout.arrive.x, layout.arrive.y, layout.arrive.z);
			ENTITY::SET_ENTITY_COORDS(ped, layout.arrive.x, layout.arrive.y, layout.arrive.z, false, false, false, false);
			ENTITY::SET_ENTITY_HEADING(ped, layout.arrive.heading);
			CAMERA::SET_GAMEPLAY_CAM_RELATIVE_HEADING(0.0f);
			for (const auto& [slot, s] : garage)
				if (slot < static_cast<int>(layout.slots.size()))
				{
					g_spawned[slot] = SpawnVehicle(s, layout.slots[slot]);
					const Vector3 v = ENTITY::GET_ENTITY_COORDS(g_spawned[slot], true);
					ml::Log("garage {}: slot {} shown as {} at {:.1f} {:.1f} {:.1f}", id, slot, g_spawned[slot], v.x, v.y, v.z);
				}
			g_inside = id;
			ml::Wait(300);
			Fade(false);
		}

		// Out of the garage; `drive` = the slot of the vehicle the player drives out with, or -1.
		void Leave(int drive)
		{
			const int id = g_inside;
			const Ped ped = PLAYER::PLAYER_PED_ID();
			Fade(true);
			Vehicle vehicle = drive >= 0 ? g_spawned[drive] : 0;
			ClearSpawned(vehicle);
			const Vector3 door = Entrance(id);
			const Place out = OutsideOnFoot(id);
			if (vehicle)
			{
				Stores(Character())[id].erase(drive);
				g_changed = true;
				MISC::CLEAR_AREA_OF_VEHICLES(door.x, door.y, door.z, 6.0f, false, false, false, false, false, false, 0);
				LoadAt(door.x, door.y, door.z);
				ENTITY::SET_ENTITY_COORDS(vehicle, door.x, door.y, door.z, false, false, false, false);
				ENTITY::SET_ENTITY_HEADING(vehicle, out.heading);
				VEHICLE::SET_VEHICLE_ON_GROUND_PROPERLY(vehicle, 5.0f);
				VEHICLE::SET_VEHICLE_ENGINE_ON(vehicle, true, true, false);
				if (!PED::IS_PED_IN_VEHICLE(ped, vehicle, false))
					PED::SET_PED_INTO_VEHICLE(ped, vehicle, -1);
				ENTITY::SET_ENTITY_AS_NO_LONGER_NEEDED(&vehicle);
				ml::Log("garage {}: slot {} driven out", id, drive);
			}
			else
			{
				LoadAt(out.x, out.y, out.z);
				ENTITY::SET_ENTITY_COORDS(ped, out.x, out.y, out.z, false, false, false, false);
				ENTITY::SET_ENTITY_HEADING(ped, out.heading);
			}
			CAMERA::SET_GAMEPLAY_CAM_RELATIVE_HEADING(0.0f);
			g_justLeft = id;
			g_inside = 0;
			RestoreInterior();
			ml::Wait(300);
			Fade(false);
		}

		void RebuildBlips(int c)
		{
			for (auto& [id, blip] : g_blips)
				HUD::REMOVE_BLIP(&blip);
			g_blips.clear();
			if (c < 0)
				return;
			for (const int id : Owned(c))
			{
				const Vector3 at = Entrance(id);
				if (!Set(at) || TierOf(id) >= 4) // apartments: the apartment's own blip (apartment.cpp)
					continue;
				const Blip blip = HUD::ADD_BLIP_FOR_COORD(at.x, at.y, at.z);
				HUD::SET_BLIP_SPRITE(blip, 357); // garage
				HUD::SET_BLIP_AS_SHORT_RANGE(blip, true);
				HUD::BEGIN_TEXT_COMMAND_SET_BLIP_NAME("STRING");
				HUD::ADD_TEXT_COMPONENT_SUBSTRING_PLAYER_NAME(Name(id).c_str());
				HUD::END_TEXT_COMMAND_SET_BLIP_NAME(blip);
				g_blips[id] = blip;
			}
		}
	}

	void Refresh()
	{
		g_refresh = true;
	}

	void EnterOnFoot(int id)
	{
		Enter(id, 0);
	}

	bool Inside()
	{
		return g_inside != 0;
	}

	bool Changed()
	{
		return g_changed;
	}

	void Commit()
	{
		for (int c = 0; c < 3; ++c)
			if (g_loaded[c])
			{
				std::ofstream out(File(c), std::ios::trunc);
				for (const auto& [id, garage] : g_stored[c])
					for (const auto& [slot, s] : garage)
						out << Serialize(id, slot, s) << '\n';
			}
		g_changed = false;
	}

	void Drop()
	{
		for (int c = 0; c < 3; ++c)
		{
			g_stored[c].clear();
			g_loaded[c] = false;
		}
		g_changed = false;
		g_spawned.clear(); // the loaded save has its own world
		g_inside = 0;
		g_refresh = true;
	}

	void Tick()
	{
		const int c = Character();
		if (c != g_character || g_refresh)
		{
			if (c != g_character && g_inside)
			{
				ClearSpawned();
				g_inside = 0;
			}
			g_character = c;
			g_refresh = false;
			RebuildBlips(c);
		}
		if (c < 0 || apartment::Inside())
			return;
		const Player player = PLAYER::PLAYER_ID();
		const Ped ped = PLAYER::PLAYER_PED_ID();
		if (PED::IS_PED_DEAD_OR_DYING(ped, true) || !PLAYER::IS_PLAYER_CONTROL_ON(player) || STREAMING::IS_PLAYER_SWITCH_IN_PROGRESS())
			return;
		const Vector3 at = ENTITY::GET_ENTITY_COORDS(ped, true);
		const bool inVehicle = PED::IS_PED_IN_ANY_VEHICLE(ped, false);
		const Vehicle vehicle = inVehicle ? PED::GET_VEHICLE_PED_IS_IN(ped, false) : 0;
		const bool driver = vehicle && VEHICLE::GET_PED_IN_VEHICLE_SEAT(vehicle, -1, false) == ped;

		if (g_inside)
		{
			const Layout& layout = LayoutFor(GarageSize(g_inside));
			// Left some other way (died, a mission, a teleport): the shown vehicles go.
			if (!Near(at, layout.arrive.x, layout.arrive.y, layout.arrive.z, 80.0f))
			{
				ClearSpawned();
				g_inside = 0;
				RestoreInterior();
				return;
			}
			if (driver)
			{
				int slot = -1;
				for (const auto& [s, v] : g_spawned)
					if (v == vehicle)
						slot = s;
				if (slot >= 0)
				{
					Help("按 ~INPUT_CONTEXT~ 開車出庫。");
					if (PAD::IS_CONTROL_JUST_PRESSED(0, 51))
						Leave(slot);
				}
				return;
			}
			if (!inVehicle)
			{
				GRAPHICS::DRAW_MARKER(1, layout.exit.x, layout.exit.y, layout.exit.z - 1.0f, 0, 0, 0, 0, 0, 0, 1.0f, 1.0f, 0.6f, 93, 182, 229, 120, false,
				    false, 2, false, nullptr, nullptr, false);
				if (Near(at, layout.exit.x, layout.exit.y, layout.exit.z, 1.5f))
				{
					const bool apartment = TierOf(g_inside) >= 4;
					Help(apartment ? "按 ~INPUT_CONTEXT~ 離開車庫\n按 ~INPUT_CONTEXT_SECONDARY~ 搭電梯回公寓" : "按 ~INPUT_CONTEXT~ 離開車庫。");
					if (PAD::IS_CONTROL_JUST_PRESSED(0, 51))
						Leave(-1);
					else if (apartment && PAD::IS_CONTROL_JUST_PRESSED(0, 52))
					{
						const int id = g_inside;
						Fade(true);
						ClearSpawned();
						g_inside = 0;
						RestoreInterior();
						apartment::Enter(id);
					}
				}
			}
			return;
		}

		for (const int id : Owned(c))
		{
			const Vector3 door = Entrance(id);
			if (!Set(door) || !Near(at, door.x, door.y, door.z, 60.0f))
				continue;
			GRAPHICS::DRAW_MARKER(1, door.x, door.y, door.z - 1.0f, 0, 0, 0, 0, 0, 0, inVehicle ? 3.0f : 1.2f, inVehicle ? 3.0f : 1.2f, 0.6f, 93, 182, 229,
			    120, false, false, 2, false, nullptr, nullptr, false);
			if (!Near(at, door.x, door.y, door.z, inVehicle ? 3.5f : 1.5f))
			{
				if (g_justLeft == id && !Near(at, door.x, door.y, door.z, 6.0f))
					g_justLeft = 0;
				continue;
			}
			if ((inVehicle && !driver) || g_justLeft == id)
				continue;
			if (PLAYER::GET_PLAYER_WANTED_LEVEL(player) > 0)
			{
				Help("被通緝時無法進入車庫。");
				continue;
			}
			const int count = static_cast<int>(Stores(c)[id].size()), size = GarageSize(id);
			Help(std::format("{}\n按 ~INPUT_CONTEXT~ {}（{}/{} 個車位）", Name(id), inVehicle ? "停入車庫" : "進入車庫", count, size));
			if (PAD::IS_CONTROL_JUST_PRESSED(0, 51))
				Enter(id, driver ? vehicle : 0);
			break;
		}
	}
}
