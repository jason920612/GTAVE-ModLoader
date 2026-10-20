// The officer's day while nobody plays them (user's choice: on duty 8-17, out 17-23, home 23-8 by the game clock).
// Switching to the officer finds them where the hour puts them, busy with something, the way the game's protagonist
// switch scenes do: at the station in uniform, out in town in their own clothes, or at home. Home is a GTA Online
// low-end apartment (property 20, Little Seoul; all low-end apartments share the interior under the map, research
// §53): its door outside and inside, and the bed, where the officer sleeps and saves.
#include "police.hpp"

#include <cmath>

namespace police::life
{
	namespace
	{
		// The apartment (property 20 of the game's property table).
		const Place kDoorOutside = {-664.00f, -853.67f, 23.43f, 0.0f};
		const Place kComeOut = {-663.31f, -853.78f, 23.71f, 93.0f};
		const Place kArriveInside = {264.57f, -1000.62f, -100.01f, 38.0f};
		const Place kDoorInside = {265.42f, -1000.75f, -100.01f, 179.0f};
		const Place kBedside = {261.21f, -1003.92f, -100.01f, 281.0f};

		struct Spot
		{
			Place at;
			const char* scenario;
		};
		const Spot kOut[] = {
		    {{-1203.0f, -1568.5f, 4.6f, 30.0f}, "WORLD_HUMAN_MUSCLE_FREE_WEIGHTS"}, // Muscle Beach
		    {{-628.0f, 239.0f, 81.9f, 90.0f}, "WORLD_HUMAN_AA_COFFEE"},             // Bean Machine, Vinewood
		    {{-565.0f, 275.0f, 83.0f, 180.0f}, "WORLD_HUMAN_SMOKING"},              // Tequi-la-la
		    {{-1640.0f, -1060.0f, 13.1f, 230.0f}, "WORLD_HUMAN_STAND_MOBILE"},      // Del Perro Pier
		    {{195.0f, -935.0f, 30.7f, 150.0f}, "WORLD_HUMAN_HANG_OUT_STREET"},      // Legion Square
		};
		const Spot kHome[] = {
		    {kBedside, "WORLD_HUMAN_STAND_IMPATIENT"},
		};

		bool g_inside = false; // the player (as the officer) is in the apartment
		Interior g_enabled = 0;

		float Distance(const Vector3& a, const Place& b)
		{
			return std::sqrt((a.x - b.x) * (a.x - b.x) + (a.y - b.y) * (a.y - b.y) + (a.z - b.z) * (a.z - b.z));
		}

		void Marker(const Place& p)
		{
			GRAPHICS::DRAW_MARKER(1, p.x, p.y, p.z - 1.0f, 0, 0, 0, 0, 0, 0, 1.0f, 1.0f, 0.6f, 93, 182, 229, 120, false, false, 2, false, nullptr, nullptr,
			    false);
		}
		void Help(const char* text)
		{
			HUD::BEGIN_TEXT_COMMAND_DISPLAY_HELP("STRING");
			HUD::ADD_TEXT_COMPONENT_SUBSTRING_PLAYER_NAME(text);
			HUD::END_TEXT_COMMAND_DISPLAY_HELP(0, false, false, -1);
		}
		bool Pressed()
		{
			return PAD::IS_CONTROL_JUST_PRESSED(0, 51); // INPUT_CONTEXT (E)
		}
		void Fade(bool out)
		{
			if (out)
			{
				CAMERA::DO_SCREEN_FADE_OUT(500);
				for (const uint64_t end = ml::TickMs() + 3000; ml::TickMs() < end && !CAMERA::IS_SCREEN_FADED_OUT();)
					ml::Wait(0);
			}
			else
				CAMERA::DO_SCREEN_FADE_IN(500);
		}
		void Move(::Ped ped, const Place& to)
		{
			ENTITY::SET_ENTITY_COORDS(ped, to.x, to.y, to.z, false, false, false, false);
			ENTITY::SET_ENTITY_HEADING(ped, to.heading);
		}
	}

	bool InApartment(const Vector3& at)
	{
		return at.z < -90.0f && Distance(at, kArriveInside) < 30.0f;
	}

	void Load(const Place& at)
	{
		STREAMING::REQUEST_COLLISION_AT_COORD(at.x, at.y, at.z);
		if (const Interior interior = INTERIOR::GET_INTERIOR_AT_COORDS(at.x, at.y, at.z))
		{
			if (INTERIOR::IS_INTERIOR_DISABLED(interior))
			{
				INTERIOR::DISABLE_INTERIOR(interior, false);
				g_enabled = interior;
			}
			INTERIOR::PIN_INTERIOR_IN_MEMORY(interior);
			for (const uint64_t end = ml::TickMs() + 5000; ml::TickMs() < end && !INTERIOR::IS_INTERIOR_READY(interior);)
				ml::Wait(0);
		}
		STREAMING::NEW_LOAD_SCENE_START_SPHERE(at.x, at.y, at.z, 50.0f, 0);
		for (const uint64_t end = ml::TickMs() + 5000; ml::TickMs() < end && !STREAMING::IS_NEW_LOAD_SCENE_LOADED();)
			ml::Wait(0);
		STREAMING::NEW_LOAD_SCENE_STOP();
	}

	Scene Now()
	{
		const int hour = CLOCK::GET_CLOCK_HOURS();
		if (hour >= 8 && hour < 17)
			return Scene::OnDuty;
		if (hour >= 17 && hour < 23)
			return Scene::Out;
		return Scene::Home;
	}

	Place Pick(Scene scene, const char*& scenario)
	{
		if (scene == Scene::OnDuty)
		{
			// At the department's station (duty.cpp).
			scenario = "WORLD_HUMAN_COP_IDLES";
			return duty::Idle();
		}
		const Spot* spots = scene == Scene::Out ? kOut : kHome;
		const int count = scene == Scene::Out ? static_cast<int>(std::size(kOut)) : static_cast<int>(std::size(kHome));
		const Spot& s = spots[MISC::GET_RANDOM_INT_IN_RANGE(0, count)];
		scenario = s.scenario;
		return s.at;
	}

	void Tick()
	{
		// Home on the map while the officer is played.
		static Blip home = 0;
		if (officer::IsPlayer() != (home != 0))
		{
			if (home)
				HUD::REMOVE_BLIP(&home);
			else
			{
				home = HUD::ADD_BLIP_FOR_COORD(kDoorOutside.x, kDoorOutside.y, kDoorOutside.z);
				HUD::SET_BLIP_SPRITE(home, 40); // safehouse
				HUD::BEGIN_TEXT_COMMAND_SET_BLIP_NAME("STRING");
				HUD::ADD_TEXT_COMPONENT_SUBSTRING_PLAYER_NAME("警察的家");
				HUD::END_TEXT_COMMAND_SET_BLIP_NAME(home);
			}
		}
		if (!officer::IsPlayer())
		{
			g_inside = false;
			return;
		}
		const ::Ped ped = PLAYER::PLAYER_PED_ID();
		const Vector3 at = ENTITY::GET_ENTITY_COORDS(ped, true);
		g_inside = InApartment(at);
		if (PED::IS_PED_IN_ANY_VEHICLE(ped, false))
			return;
		if (!g_inside && Distance(at, kDoorOutside) < 25.0f)
		{
			Marker(kDoorOutside);
			if (Distance(at, kDoorOutside) < 1.2f)
			{
				Help("按 ~INPUT_CONTEXT~ 回家");
				if (Pressed())
				{
					Fade(true);
					Load(kArriveInside);
					Move(ped, kArriveInside);
					CAMERA::SET_GAMEPLAY_CAM_RELATIVE_HEADING(0.0f);
					ml::Wait(300);
					Fade(false);
				}
			}
		}
		if (g_inside)
		{
			Marker(kDoorInside);
			Marker(kBedside);
			if (Distance(at, kDoorInside) < 1.2f)
			{
				Help("按 ~INPUT_CONTEXT~ 出門");
				if (Pressed())
				{
					Fade(true);
					Load(kComeOut);
					Move(ped, kComeOut);
					CAMERA::SET_GAMEPLAY_CAM_RELATIVE_HEADING(0.0f);
					ml::Wait(300);
					Fade(false);
				}
			}
			else if (Distance(at, kBedside) < 1.2f)
			{
				Help("按 ~INPUT_CONTEXT~ 睡覺並存檔");
				if (Pressed())
					officer::SaveForOfficer();
			}
		}
	}
}
