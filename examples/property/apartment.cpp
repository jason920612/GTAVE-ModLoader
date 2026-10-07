// Apartments of the properties the player owns: the GTA Online apartment interiors (story mode has them), a door
// marker outside, and inside: the front door (leave, or take the elevator to the garage), and the bedroom (sleep and
// save with the game's save menu, the game's own wardrobe). Positions come from the property table
// (research/phase0.md §32).
#define NOMINMAX
#include <cstring>
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

		// ---- the game's own wardrobe -----------------------------------------------------------
		// wardrobe_sp has a generic wardrobe (argument 7; research/phase0.md §32): stand position and heading come from the
		// launch arguments, and it opens when its trigger area check passes. That check is answered "yes" (for this script
		// only) while the player stands at the apartment's wardrobe; the thread is stopped when the player leaves.
		constexpr uint64_t kIsEntityInAngledArea = 0x51210CED3DA1C78AULL;
		int g_wardrobe = 0;    // wardrobe_sp thread started here
		Place g_wardrobeAt{};  // its stand position

		int64_t FloatArg(float f)
		{
			uint32_t bits;
			std::memcpy(&bits, &f, 4);
			return bits;
		}

		// Where Online puts the player at the apartment's wardrobe, by property type: shop_controller @2614077, run offline
		// with the interiors' metadata (GET_BASE_ELEMENT_LOCATION_FROM_METADATA_BLOCK element 40) taken from the game;
		// z raised to the player's centre as wardrobe_sp expects (research/phase0.md §32).
		bool WardrobeAt(int id, Place& out)
		{
			static const std::map<int, Place> table{
		    {1, {-796.220f, 331.850f, 201.429f, -85.52f}},
		    {2, {-759.797f, 325.402f, 170.612f, 94.48f}},
		    {3, {-759.887f, 325.406f, 217.066f, 94.48f}},
		    {4, {-796.687f, 332.242f, 153.810f, -85.52f}},
		    {5, {-265.043f, -946.802f, 71.040f, 164.48f}},
		    {6, {-278.143f, -961.538f, 86.319f, -15.52f}},
		    {7, {-1468.488f, -537.865f, 63.365f, -50.52f}},
		    {8, {350.741f, -993.622f, -99.202f, 179.61f}},
		    {9, {350.741f, -993.622f, -99.202f, 179.61f}},
		    {10, {350.741f, -993.622f, -99.202f, 179.61f}},
		    {11, {350.741f, -993.622f, -99.202f, 179.61f}},
		    {12, {350.741f, -993.622f, -99.202f, 179.61f}},
		    {13, {350.741f, -993.622f, -99.202f, 179.61f}},
		    {14, {350.741f, -993.622f, -99.202f, 179.61f}},
		    {15, {350.741f, -993.622f, -99.202f, 179.61f}},
		    {16, {350.741f, -993.622f, -99.202f, 179.61f}},
		    {17, {259.818f, -1003.794f, -99.009f, 307.16f}},
		    {18, {259.818f, -1003.794f, -99.009f, 307.16f}},
		    {19, {259.818f, -1003.794f, -99.009f, 307.16f}},
		    {20, {259.818f, -1003.794f, -99.009f, 307.16f}},
		    {21, {259.818f, -1003.794f, -99.009f, 307.16f}},
		    {22, {259.818f, -1003.794f, -99.009f, 307.16f}},
		    {23, {259.818f, -1003.794f, -99.009f, 307.16f}},
		    {34, {-1468.488f, -537.865f, 50.737f, -50.52f}},
		    {35, {-887.814f, -443.981f, 120.343f, 122.04f}},
		    {36, {-910.708f, -445.961f, 115.416f, -58.83f}},
		    {37, {-900.073f, -432.839f, 89.270f, -148.60f}},
		    {38, {-38.929f, -583.179f, 83.923f, -105.52f}},
		    {39, {-17.241f, -586.860f, 94.041f, 164.48f}},
		    {40, {-902.992f, -369.223f, 79.289f, 121.41f}},
		    {41, {-927.054f, -382.012f, 103.249f, -58.07f}},
		    {42, {-618.466f, 56.828f, 101.836f, -85.52f}},
		    {43, {-582.900f, 50.490f, 87.435f, 94.48f}},
		    {61, {-793.363f, 326.289f, 210.797f, -7.76f}},
		    {62, {-1449.641f, -548.914f, 72.844f, 117.24f}},
		    {63, {-903.947f, -363.649f, 113.074f, -160.76f}},
		    {64, {-594.710f, 56.309f, 97.000f, 172.24f}},
		    {65, {-38.294f, -589.753f, 78.830f, -27.88f}},
		    {66, {350.741f, -993.622f, -99.202f, 179.61f}},
		    {67, {350.741f, -993.622f, -99.202f, 179.61f}},
		    {68, {350.741f, -993.622f, -99.202f, 179.61f}},
		    {69, {350.741f, -993.622f, -99.202f, 179.61f}},
		    {70, {259.818f, -1003.794f, -99.009f, 307.16f}},
		    {71, {259.818f, -1003.794f, -99.009f, 307.16f}},
		    {72, {259.818f, -1003.794f, -99.009f, 307.16f}},
		    {73, {-167.393f, 487.737f, 133.844f, -174.21f}},
		    {74, {334.276f, 428.485f, 145.571f, 111.29f}},
		    {75, {-767.340f, 610.911f, 140.331f, 103.29f}},
		    {76, {-671.475f, 587.296f, 141.570f, -144.71f}},
		    {77, {122.211f, 548.765f, 180.497f, 174.60f}},
		    {78, {-571.145f, 649.736f, 142.032f, 154.10f}},
		    {79, {-743.374f, 582.299f, 142.461f, 139.60f}},
		    {80, {-855.198f, 679.968f, 149.053f, 173.10f}},
		    {81, {-1286.031f, 438.045f, 94.095f, 168.60f}},
		    {82, {374.508f, 411.506f, 142.101f, 154.60f}},
		    {83, {-797.775f, 327.133f, 190.714f, -2.91f}},
		    {84, {-797.775f, 327.133f, 220.438f, -2.91f}},
		    {85, {-763.231f, 330.616f, 199.486f, 177.09f}},
			};
			const int64_t* e = Entry(id);
			const auto it = e ? table.find(static_cast<int>(e[31])) : table.end();
			if (it == table.end())
				return false;
			out = it->second;
			return true;
		}

		void StartWardrobe(const Place& at)
		{
			if (g_wardrobe && SCRIPT::IS_THREAD_ACTIVE(g_wardrobe))
				return;
			SCRIPT::REQUEST_SCRIPT("wardrobe_sp");
			for (int i = 0; i < 200 && !SCRIPT::HAS_SCRIPT_LOADED("wardrobe_sp"); ++i)
				ml::Wait(10);
			int64_t args[5] = {7, FloatArg(at.x), FloatArg(at.y), FloatArg(at.z), FloatArg(at.heading)};
			g_wardrobeAt = at;
			g_wardrobe = BUILTIN::START_NEW_SCRIPT_WITH_ARGS("wardrobe_sp", reinterpret_cast<Any*>(args), 5, 2324);
			SCRIPT::SET_SCRIPT_AS_NO_LONGER_NEEDED("wardrobe_sp");
		}
		void StopWardrobe()
		{
			if (g_wardrobe && SCRIPT::IS_THREAD_ACTIVE(g_wardrobe))
				SCRIPT::TERMINATE_THREAD(g_wardrobe);
			g_wardrobe = 0;
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
			StopWardrobe();
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

	void RegisterOverrides()
	{
		ml::scripts::OverrideNative("wardrobe_sp", kIsEntityInAngledArea, [](ml::scripts::NativeCall& call) {
			if (g_wardrobe && g_inside)
			{
				const Vector3 at = ENTITY::GET_ENTITY_COORDS(PLAYER::PLAYER_PED_ID(), true);
				call.Return<int64_t>(Near(at, g_wardrobeAt.x, g_wardrobeAt.y, g_wardrobeAt.z, 1.5f) ? 1 : 0);
			}
			else
				call.CallOriginal();
		});
	}

	void Tick()
	{
		const int c = ml::game::CharacterIndex();
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
				StopWardrobe();
				g_inside = 0; // left some other way
				RestoreInterior();
				return;
			}
			const Place door = FrontDoor(id), bed = Bedroom(id);
			Marker(door.x, door.y, door.z, 1.0f);
			// The wardrobe opens by itself once started (its own prompt); started when the player reaches the bedroom.
			// The game's own wardrobe (its own "change outfit" prompt), started when the player comes near it.
			if (Place wardrobe; WardrobeAt(id, wardrobe) && Near(at, wardrobe.x, wardrobe.y, wardrobe.z, 8.0f))
				StartWardrobe(wardrobe);
			Marker(bed.x, bed.y, bed.z, 1.0f);
			if (Near(at, door.x, door.y, door.z, 1.2f))
			{
				Help("按 ~INPUT_CONTEXT~ 出門\n按 ~INPUT_CONTEXT_SECONDARY~ 搭電梯到車庫");
				if (PAD::IS_CONTROL_JUST_PRESSED(0, 51))
					LeaveTo(Outside(id));
				else if (PAD::IS_CONTROL_JUST_PRESSED(0, 52))
				{
					ml::Log("apartment {}: elevator to the garage", id);
					StopWardrobe();
					g_inside = 0;
					garage::EnterOnFoot(id);
				}
			}
			else if (Near(at, bed.x, bed.y, bed.z, 1.2f))
			{
				// E belongs to the wardrobe (its own prompt); sleeping is on the second key.
				if (!HUD::IS_HELP_MESSAGE_BEING_DISPLAYED())
					Help("按 ~INPUT_CONTEXT_SECONDARY~ 睡覺並存檔");
				if (PAD::IS_CONTROL_JUST_PRESSED(0, 52))
					Sleep();
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
