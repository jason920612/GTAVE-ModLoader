// World helpers shared by the garages and the apartments.
#include "property.hpp"

namespace property
{
	namespace
	{
		// Interiors story mode keeps disabled (the 2 and 6 car garages): enabled while the player is in one, then put back.
		Interior g_enabled = 0;
	}

	bool Near(const Vector3& a, float x, float y, float z, float radius)
	{
		const float dx = a.x - x, dy = a.y - y, dz = a.z - z;
		return dx * dx + dy * dy + dz * dz < radius * radius;
	}

	void Fade(bool out)
	{
		if (out)
		{
			CAMERA::DO_SCREEN_FADE_OUT(500);
			for (int i = 0; i < 60 && !CAMERA::IS_SCREEN_FADED_OUT(); ++i)
				ml::Wait(10);
		}
		else
			CAMERA::DO_SCREEN_FADE_IN(500);
	}

	void RestoreInterior()
	{
		if (g_enabled)
			INTERIOR::DISABLE_INTERIOR(g_enabled, true);
		g_enabled = 0;
	}

	void LoadAt(float x, float y, float z)
	{
		STREAMING::REQUEST_COLLISION_AT_COORD(x, y, z);
		const Interior interior = INTERIOR::GET_INTERIOR_AT_COORDS(x, y, z);
		if (interior)
		{
			if (INTERIOR::IS_INTERIOR_DISABLED(interior))
			{
				RestoreInterior();
				INTERIOR::DISABLE_INTERIOR(interior, false);
				g_enabled = interior;
			}
			INTERIOR::PIN_INTERIOR_IN_MEMORY(interior);
			for (int i = 0; i < 100 && !INTERIOR::IS_INTERIOR_READY(interior); ++i)
				ml::Wait(50);
		}
		STREAMING::NEW_LOAD_SCENE_START_SPHERE(x, y, z, 50.0f, 0);
		for (int i = 0; i < 100 && !STREAMING::IS_NEW_LOAD_SCENE_LOADED(); ++i)
			ml::Wait(50);
		STREAMING::NEW_LOAD_SCENE_STOP();
	}

	void MovePlayer(const Place& to)
	{
		const Ped ped = PLAYER::PLAYER_PED_ID();
		ENTITY::SET_ENTITY_COORDS(ped, to.x, to.y, to.z, false, false, false, false);
		ENTITY::SET_ENTITY_HEADING(ped, to.heading);
		CAMERA::SET_GAMEPLAY_CAM_RELATIVE_HEADING(0.0f);
	}

	void Marker(float x, float y, float z, float size)
	{
		GRAPHICS::DRAW_MARKER(1, x, y, z - 1.0f, 0, 0, 0, 0, 0, 0, size, size, 0.6f, 93, 182, 229, 120, false, false, 2, false, nullptr, nullptr, false);
	}
}
