// On and off duty (user's design): the officer goes on duty at their department's station, in the locker room, where
// they change into the uniform and take their equipment, and picks a patrol car in the station's lot. Off duty again
// at the locker. Being in uniform is being on duty: the switch scene at the station puts the officer there on duty too.
//
// Mission Row (LSPD): the story-mode interior (30978) holds the lobby and the rooms behind it. The locker room is the
// bare briefing room: from the lobby through the double door marked CELLS / BRIEFING, down the corridor, the blue door
// on the right; the mod stands lockers in it. The corridor ends at a locked door marked GARAGE, which the mod makes a
// way (with a fade) to the lot behind the station, by the parking ramp; the game parks its own patrol cars there.
// Stations of the other departments come later; until then they use Mission Row.
#include "police.hpp"

#include <cmath>
#include <tuple>
#include <vector>

namespace police::duty
{
	namespace
	{
		struct Station
		{
			const char* name;
			Place locker;             // where the officer stands to change
			Place doorInside;         // the way out to the lot ...
			Place doorOutside;        // ... and back in
			Place lot;                // where they stand to pick a car
			Place bay;                // where the picked car is put
			std::vector<const char*> cars;
			std::vector<Place> lockers; // locker props the mod puts in the (bare) room
			size_t own;                 // the officer's (open) one among them
		};
		const Station kMissionRow = {
		    "Mission Row",
		    {438.4f, -995.6f, 30.69f, 180.0f},
		    {445.5f, -996.0f, 30.69f, 0.0f},    // before the GARAGE door, arriving facing up the corridor
		    {450.0f, -1012.6f, 28.49f, 180.0f}, // before the parking ramp's shutters, arriving facing the lot
		    {446.0f, -1015.5f, 28.49f, 180.0f},
		    {443.0f, -1019.0f, 28.5f, 270.0f},
		    {"police", "police2", "police3", "police4", "policeb"},
		    // Along the room's south wall (x 435.9-442.8, y -996.8 to -989.7).
		    // A row along the room's south wall (x 435.9-442.8, y -996.8 to -989.7), the officer's in front of the marker.
		    {{436.54f, -996.3f, 29.69f, 180.0f}, {437.16f, -996.3f, 29.69f, 180.0f}, {437.78f, -996.3f, 29.69f, 180.0f}, {438.40f, -996.3f, 29.69f, 180.0f},
		        {439.02f, -996.3f, 29.69f, 180.0f}, {439.64f, -996.3f, 29.69f, 180.0f}, {440.26f, -996.3f, 29.69f, 180.0f}},
		    3,
		};

		const Station& Current()
		{
			return kMissionRow; // look::Department(): own stations per department come later
		}

		// The equipment (user's list): a pistol, the taser, the baton and a flashlight.
		const char* const kEquipment[] = {"WEAPON_COMBATPISTOL", "WEAPON_STUNGUN", "WEAPON_NIGHTSTICK", "WEAPON_FLASHLIGHT"};

		std::vector<Object> g_props; // the station's lockers while the officer is near
		Vehicle g_car = 0;     // the patrol car picked in the lot
		Blip g_carBlip = 0;
		// Picking a car: the choice stands in the bay while the player looks through them.
		bool g_picking = false;
		size_t g_choice = 0;
		Vehicle g_preview = 0;

		float Distance(const Vector3& a, const Place& b)
		{
			return std::sqrt((a.x - b.x) * (a.x - b.x) + (a.y - b.y) * (a.y - b.y) + (a.z - b.z) * (a.z - b.z));
		}
		void Marker(const Place& p)
		{
			GRAPHICS::DRAW_MARKER(1, p.x, p.y, p.z - 1.0f, 0, 0, 0, 0, 0, 0, 1.0f, 1.0f, 0.6f, 93, 182, 229, 120, false, false, 2, false, nullptr, nullptr,
			    false);
		}
		// A text component holds about 99 bytes: a second line (after ~n~) goes in a second one.
		void Help(const std::string& text)
		{
			const size_t split = text.find("~n~");
			HUD::BEGIN_TEXT_COMMAND_DISPLAY_HELP(split == std::string::npos ? "STRING" : "TWOSTRINGS");
			HUD::ADD_TEXT_COMPONENT_SUBSTRING_PLAYER_NAME(text.substr(0, split).c_str());
			if (split != std::string::npos)
				HUD::ADD_TEXT_COMPONENT_SUBSTRING_PLAYER_NAME(text.substr(split).c_str());
			HUD::END_TEXT_COMMAND_DISPLAY_HELP(0, false, false, -1);
		}
		bool Pressed(int control)
		{
			return PAD::IS_CONTROL_JUST_PRESSED(0, control);
		}
		constexpr int kContext = 51;   // INPUT_CONTEXT (E)
		constexpr int kLeft = 174;     // INPUT_CELLPHONE_LEFT (←)
		constexpr int kRight = 175;    // INPUT_CELLPHONE_RIGHT (→)
		constexpr int kCancel = 177;   // INPUT_CELLPHONE_CANCEL (Backspace)

		void Fade(bool out)
		{
			if (out)
			{
				CAMERA::DO_SCREEN_FADE_OUT(400);
				for (const uint64_t end = ml::TickMs() + 3000; ml::TickMs() < end && !CAMERA::IS_SCREEN_FADED_OUT();)
					ml::Wait(0);
			}
			else
				CAMERA::DO_SCREEN_FADE_IN(400);
		}

		bool Exists(Entity e)
		{
			return e && ENTITY::DOES_ENTITY_EXIST(e);
		}
		void Delete(Vehicle& v)
		{
			if (Exists(v))
			{
				ENTITY::SET_ENTITY_AS_MISSION_ENTITY(v, true, true);
				VEHICLE::DELETE_VEHICLE(&v);
			}
			v = 0;
		}

		// The game's name of a car model, in the game's language.
		std::string CarName(const char* model)
		{
			const char* label = VEHICLE::GET_DISPLAY_NAME_FROM_VEHICLE_MODEL(MISC::GET_HASH_KEY(model));
			const char* text = label ? HUD::GET_FILENAME_FOR_AUDIO_CONVERSATION(label) : nullptr;
			return text && *text && std::string(text) != "NULL" ? text : model;
		}

		Vehicle Spawn(const char* model, const Place& at)
		{
			const Hash hash = MISC::GET_HASH_KEY(model);
			const auto loaded = ml::LoadModel(hash);
			if (!loaded)
				return 0;
			// The bay cleared, the game's own parked patrol cars too (CLEAR_AREA_OF_VEHICLES leaves those).
			MISC::CLEAR_AREA(at.x, at.y, at.z, 4.0f, true, false, true, false);
			const Vehicle v = VEHICLE::CREATE_VEHICLE(hash, at.x, at.y, at.z + 0.3f, at.heading, false, false, false);
			if (v)
			{
				ENTITY::SET_ENTITY_AS_MISSION_ENTITY(v, true, true);
				VEHICLE::SET_VEHICLE_ON_GROUND_PROPERLY(v, 5.0f);
				VEHICLE::SET_VEHICLE_DOORS_LOCKED(v, 1);
			}
			return v;
		}

		void ShowPreview()
		{
			Delete(g_preview);
			g_preview = Spawn(Current().cars[g_choice], Current().bay);
		}

		// The car already picked stands aside (out of sight, out of the way) while another is looked at.
		void Aside(bool aside)
		{
			if (!Exists(g_car))
				return;
			ENTITY::SET_ENTITY_VISIBLE(g_car, !aside, false);
			ENTITY::SET_ENTITY_COLLISION(g_car, !aside, true);
			ENTITY::FREEZE_ENTITY_POSITION(g_car, aside);
		}

		void StartPicking()
		{
			g_picking = true;
			Aside(true);
			ShowPreview();
		}

		// Ends picking: the car looked at becomes the patrol car (`keep`), or the old one comes back.
		void StopPicking(bool keep)
		{
			g_picking = false;
			if (keep && Exists(g_preview))
			{
				Delete(g_car);
				g_car = g_preview;
				g_preview = 0;
				ml::Log("duty: patrol car {}", Current().cars[g_choice]);
				return;
			}
			Delete(g_preview);
			Aside(false);
		}

		void Pick(const std::vector<const char*>& cars)
		{
			if (Pressed(kLeft) || Pressed(kRight))
			{
				g_choice = (g_choice + (Pressed(kRight) ? 1 : cars.size() - 1)) % cars.size();
				ShowPreview();
			}
			Help("~INPUT_CELLPHONE_LEFT~~INPUT_CELLPHONE_RIGHT~ " + CarName(cars[g_choice]) + "~n~~INPUT_CONTEXT~ 確定 ~INPUT_CELLPHONE_CANCEL~ 取消");
			if (Pressed(kContext) && Exists(g_preview))
				StopPicking(true);
			else if (Pressed(kCancel))
				StopPicking(false);
		}

		// The lockers stand while the officer is played near the station.
		void Props(bool show)
		{
			if (show == !g_props.empty())
				return;
			for (Object& o : g_props)
				if (Exists(o))
				{
					ENTITY::SET_ENTITY_AS_MISSION_ENTITY(o, true, true);
					OBJECT::DELETE_OBJECT(&o);
				}
			g_props.clear();
			if (!show)
				return;
			const Hash closed = MISC::GET_HASH_KEY("p_cs_locker_01_s"), open = MISC::GET_HASH_KEY("p_cs_locker_01");
			const auto loadedClosed = ml::LoadModel(closed);
			const auto loadedOpen = ml::LoadModel(open);
			if (!loadedClosed || !loadedOpen)
				return;
			const Station& station = Current();
			for (size_t i = 0; i < station.lockers.size(); ++i)
				if (const Place& at = station.lockers[i]; const Object o = OBJECT::CREATE_OBJECT_NO_OFFSET(i == station.own ? open : closed, at.x, at.y, at.z, false, false, false, 0))
				{
					ENTITY::SET_ENTITY_HEADING(o, at.heading);
					ENTITY::FREEZE_ENTITY_POSITION(o, true);
					g_props.push_back(o);
				}
		}

		void CarBlip(bool show)
		{
			if (show == (g_carBlip != 0) && (!show || HUD::DOES_BLIP_EXIST(g_carBlip)))
				return;
			if (g_carBlip)
				HUD::REMOVE_BLIP(&g_carBlip);
			g_carBlip = 0;
			if (show)
			{
				g_carBlip = HUD::ADD_BLIP_FOR_ENTITY(g_car);
				HUD::SET_BLIP_SPRITE(g_carBlip, 56); // police car
				HUD::SET_BLIP_COLOUR(g_carBlip, 3);
				HUD::BEGIN_TEXT_COMMAND_SET_BLIP_NAME("STRING");
				HUD::ADD_TEXT_COMPONENT_SUBSTRING_PLAYER_NAME("巡邏車");
				HUD::END_TEXT_COMMAND_SET_BLIP_NAME(g_carBlip);
			}
		}
	}

	void Equip(::Ped ped, bool on)
	{
		for (const char* weapon : kEquipment)
		{
			const Hash hash = MISC::GET_HASH_KEY(weapon);
			if (on)
				WEAPON::GIVE_WEAPON_TO_PED(ped, hash, 120, true, false);
			else
				WEAPON::REMOVE_WEAPON_FROM_PED(ped, hash);
		}
	}

	bool OnDuty()
	{
		return officer::InUniform();
	}

	void Forget()
	{
		g_car = g_preview = 0;
		g_carBlip = 0;
		g_picking = false;
		g_props.clear();
	}

	void Tick()
	{
		const bool player = officer::IsPlayer();
		// The station on the map while the officer is played.
		static Blip stationBlip = 0;
		if (player != (stationBlip != 0))
		{
			if (stationBlip)
				HUD::REMOVE_BLIP(&stationBlip);
			else
			{
				const Place& at = Current().locker;
				stationBlip = HUD::ADD_BLIP_FOR_COORD(at.x, at.y, at.z);
				HUD::SET_BLIP_SPRITE(stationBlip, 60); // police station
				HUD::SET_BLIP_AS_SHORT_RANGE(stationBlip, true);
				HUD::BEGIN_TEXT_COMMAND_SET_BLIP_NAME("STRING");
				HUD::ADD_TEXT_COMPONENT_SUBSTRING_PLAYER_NAME("警局（置物間）");
				HUD::END_TEXT_COMMAND_SET_BLIP_NAME(stationBlip);
			}
		}
		CarBlip(player && OnDuty() && Exists(g_car) && !ENTITY::IS_ENTITY_DEAD(g_car, false) &&
		        !PED::IS_PED_IN_VEHICLE(PLAYER::PLAYER_PED_ID(), g_car, false));
		if (!player)
		{
			Props(false);
			if (g_picking)
				StopPicking(false);
			return;
		}
		const ::Ped ped = PLAYER::PLAYER_PED_ID();
		const Station& station = Current();
		const Vector3 at = ENTITY::GET_ENTITY_COORDS(ped, true);
		Props(player && Distance(at, station.locker) < 80.0f);
		if (PED::IS_PED_IN_ANY_VEHICLE(ped, false))
			return;

		// The locker: on duty or off.
		if (Distance(at, station.locker) < 25.0f)
		{
			Marker(station.locker);
			if (Distance(at, station.locker) < 1.2f)
			{
				const bool on = OnDuty();
				Help(on ? "按 ~INPUT_CONTEXT~ 換回便服下班" : "按 ~INPUT_CONTEXT~ 換上制服、領裝備上班");
				if (Pressed(kContext))
				{
					Fade(true);
					officer::Dress(!on);
					ml::Wait(300);
					Fade(false);
					if (on)
						ml::Notify("已下班");
					else
						ml::Notify("已上班：{}。巡邏車在局後方停車場", station.name);
					ml::Log("duty: {}", on ? "off" : "on");
				}
			}
		}

		// The way between the corridor and the lot.
		for (const auto& [from, to, text] : {std::tuple{station.doorInside, station.doorOutside, "按 ~INPUT_CONTEXT~ 去停車場"},
		         std::tuple{station.doorOutside, station.doorInside, "按 ~INPUT_CONTEXT~ 進警局"}})
		{
			if (Distance(at, from) >= 25.0f)
				continue;
			Marker(from);
			if (Distance(at, from) < 1.2f)
			{
				Help(text);
				if (Pressed(kContext))
				{
					Fade(true);
					life::Load(to);
					ENTITY::SET_ENTITY_COORDS(ped, to.x, to.y, to.z, false, false, false, false);
					ENTITY::SET_ENTITY_HEADING(ped, to.heading);
					CAMERA::SET_GAMEPLAY_CAM_RELATIVE_HEADING(0.0f);
					ml::Wait(300);
					Fade(false);
				}
			}
		}

		// The lot: a patrol car while on duty.
		if (OnDuty() && Distance(at, station.lot) < 30.0f)
		{
			if (g_picking)
				Pick(station.cars);
			else
			{
				Marker(station.lot);
				if (Distance(at, station.lot) < 1.2f)
				{
					Help("按 ~INPUT_CONTEXT~ 選一台巡邏車");
					if (Pressed(kContext))
						StartPicking();
				}
			}
		}
		else if (g_picking)
			StopPicking(false);
	}
}
