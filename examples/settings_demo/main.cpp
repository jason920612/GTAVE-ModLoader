// Example mod: settings only. Shows how a mod reads values the player sets in the pause menu.
#include <iterator>

#include <modloader/natives.hpp>

ML_MOD_INFO("Settings Demo", "1.0.0", "ModLoader", "Pause menu settings example")

namespace
{
	ml::Setting g_neverWanted;
	ml::Setting g_density;
	ml::Setting g_weather;

	// Index 0 keeps the game's own weather.
	constexpr const char* kWeathers[] = {nullptr, "CLEAR", "RAIN", "THUNDER", "FOGGY", "XMAS"};

	void ApplyWeather(int index)
	{
		if (index > 0 && index < static_cast<int>(std::size(kWeathers)))
			MISC::SET_WEATHER_TYPE_NOW_PERSIST(kWeathers[index]);
		else
			MISC::CLEAR_WEATHER_TYPE_PERSIST();
	}
}

extern "C" __declspec(dllexport) int MLOnLoad(const MLApi* api, const MLContext* ctx)
{
	ml::Init(api, ctx);
	g_neverWanted = ml::AddToggle("neverWanted", "永不通緝", false);
	g_density = ml::AddSlider("density", "路人與車流密度", 10);
	g_weather = ml::AddList("weather", "天氣", {"不變", "晴天", "雨天", "雷雨", "大霧", "下雪"}, 0);
	return 1;
}

extern "C" __declspec(dllexport) void MLMain()
{
	int lastNeverWanted = -1, lastDensity = -1, lastWeather = -1;
	for (;;)
	{
		const Player player = PLAYER::PLAYER_ID();
		if (g_neverWanted)
			PLAYER::CLEAR_PLAYER_WANTED_LEVEL(player);

		const float density = g_density.Value() / 10.0f;
		PED::SET_PED_DENSITY_MULTIPLIER_THIS_FRAME(density);
		VEHICLE::SET_VEHICLE_DENSITY_MULTIPLIER_THIS_FRAME(density);

		if (g_weather.Value() != lastWeather)
		{
			lastWeather = g_weather.Value();
			ApplyWeather(lastWeather);
			ml::Log("weather {}", lastWeather);
		}

		if (g_neverWanted.Value() != lastNeverWanted || g_density.Value() != lastDensity)
		{
			lastNeverWanted = g_neverWanted.Value();
			lastDensity = g_density.Value();
			ml::Log("never wanted {}, density {}", lastNeverWanted, lastDensity);
		}
		ml::Wait(0);
	}
}
