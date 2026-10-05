// Example mod: settings only. Shows how a mod reads values the player sets in the pause menu.
#include <modloader/natives.hpp>

ML_MOD_INFO("Settings Demo", "1.0.0", "ModLoader", "Pause menu settings example")

namespace
{
	ml::Setting g_neverWanted;
	ml::Setting g_density;
}

extern "C" __declspec(dllexport) int MLOnLoad(const MLApi* api, const MLContext* ctx)
{
	ml::Init(api, ctx);
	g_neverWanted = ml::AddToggle("neverWanted", "永不通緝", false);
	g_density = ml::AddSlider("density", "路人與車流密度", 10);
	return 1;
}

extern "C" __declspec(dllexport) void MLMain()
{
	int lastNeverWanted = -1, lastDensity = -1;
	for (;;)
	{
		const Player player = PLAYER::PLAYER_ID();
		if (g_neverWanted)
			PLAYER::CLEAR_PLAYER_WANTED_LEVEL(player);

		const float density = g_density.Value() / 10.0f;
		PED::SET_PED_DENSITY_MULTIPLIER_THIS_FRAME(density);
		VEHICLE::SET_VEHICLE_DENSITY_MULTIPLIER_THIS_FRAME(density);

		if (g_neverWanted.Value() != lastNeverWanted || g_density.Value() != lastDensity)
		{
			lastNeverWanted = g_neverWanted.Value();
			lastDensity = g_density.Value();
			ml::Log("never wanted {}, density {}", lastNeverWanted, lastDensity);
		}
		ml::Wait(0);
	}
}
