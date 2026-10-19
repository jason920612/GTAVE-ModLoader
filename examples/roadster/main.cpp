// Tesla Roadster (second generation): the acceleration of the three trims. The car itself is an add-on vehicle pack
// (ModLoader\mods\roadster\dlc.rpf, models tr22 / tr22t / tr22r, not part of this repository); this mod makes it pull
// like the real one, and shop.json puts it on Legendary Motorsport (property mod).
//
// The game's tyres cannot launch a car at 3-5 g, so while the player drives one of the trims with the throttle down
// and all wheels on the ground, the mod adds whatever forward speed the engine did not reach this frame to follow the
// trim's acceleration curve: the official 0-60 mph time up to 60 mph, constant power above it, fading out at the top
// speed. It never slows the car, and a sudden loss of speed (a crash, braking) restarts the curve from the new speed.
#define NOMINMAX
#include <Windows.h>

#include <algorithm>
#include <cmath>

#include <modloader/natives.hpp>

ML_MOD_INFO("Tesla Roadster", "1.0.0", "ModLoader", "Roadster Performance / Thrust / Rocket acceleration")

namespace
{
	constexpr float kSixtyMph = 26.8224f; // m/s

	struct Trim
	{
		const char* model;
		const char* name;
		float zeroToSixty; // s (Tesla's figures)
		float topSpeed;    // m/s
	};
	const Trim kTrims[] = {
	    {"tr22", "Roadster Performance", 1.9f, 103.0f},
	    {"tr22t", "Roadster Thrust", 0.81f, 103.0f},
	    {"tr22r", "Roadster Rocket", 0.59f, 103.0f},
	};

	ml::Setting g_enabled;
	ml::Setting g_showTimes;

	const Trim* TrimOf(Hash model)
	{
		for (const Trim& t : kTrims)
			if (MISC::GET_HASH_KEY(t.model) == model)
				return &t;
		return nullptr;
	}

	// Acceleration the trim aims for at forward speed v (m/s).
	float Target(const Trim& t, float v)
	{
		const float launch = kSixtyMph / t.zeroToSixty;
		const float power = v > kSixtyMph ? launch * kSixtyMph / v : launch;
		const float r = std::clamp(v / t.topSpeed, 0.0f, 1.0f);
		return power * (1.0f - r * r * r * r);
	}

	// A 0-60 mph run: starts when the car pulls away from (almost) standing still.
	struct Run
	{
		bool timing = false;
		uint64_t start = 0;
		float lastTime = 0.0f;
	} g_run;

	void Time(const Trim& t, float v, float throttle)
	{
		if (!g_run.timing && v < 0.3f && throttle > 0.9f)
		{
			g_run.timing = true;
			g_run.start = ml::TickMs();
		}
		else if (g_run.timing && (throttle < 0.9f || v < -0.5f))
			g_run.timing = false;
		else if (g_run.timing && v >= kSixtyMph)
		{
			g_run.timing = false;
			const float s = (ml::TickMs() - g_run.start) / 1000.0f;
			ml::Log("{}: 0-60 mph in {:.2f} s (target {:.2f} s)", t.name, s, t.zeroToSixty);
			if (g_showTimes)
				ml::Notify("{}：0–96 km/h {:.2f} 秒", t.name, s);
		}
	}

	float g_expected = -1.0f; // forward speed the car should have now (m/s), < 0 = no curve running
	Vehicle g_vehicle = 0;

	void Tick()
	{
		const Ped player = PLAYER::PLAYER_PED_ID();
		const Vehicle v = PED::IS_PED_IN_ANY_VEHICLE(player, false) ? PED::GET_VEHICLE_PED_IS_IN(player, false) : 0;
		const Trim* trim = v && VEHICLE::GET_PED_IN_VEHICLE_SEAT(v, -1, false) == player ? TrimOf(ENTITY::GET_ENTITY_MODEL(v)) : nullptr;
		if (!trim || !g_enabled)
		{
			g_expected = -1.0f;
			g_run.timing = false;
			return;
		}
		if (v != g_vehicle)
		{
			g_vehicle = v;
			g_expected = -1.0f;
		}
		const float speed = ENTITY::GET_ENTITY_SPEED_VECTOR(v, true).y;
		const float throttle = PAD::GET_CONTROL_NORMAL(0, 71); // INPUT_VEH_ACCELERATE
		const float brake = PAD::GET_CONTROL_NORMAL(0, 72);    // INPUT_VEH_BRAKE
		Time(*trim, speed, throttle);

		const float dt = std::min(MISC::GET_FRAME_TIME(), 0.05f);
		if (throttle < 0.05f || brake > 0.05f || speed < -0.5f || !VEHICLE::IS_VEHICLE_ON_ALL_WHEELS(v) || dt <= 0.0f)
		{
			g_expected = -1.0f;
			return;
		}
		// Restart the curve from the actual speed when the car lost speed it should have had (or just started).
		if (g_expected < 0.0f || speed < g_expected - 2.0f)
			g_expected = std::max(speed, 0.0f);
		g_expected += Target(*trim, g_expected) * throttle * dt;
		const float add = g_expected - speed;
		if (add > 0.0f)
		{
			const Vector3 vel = ENTITY::GET_ENTITY_VELOCITY(v), fwd = ENTITY::GET_ENTITY_FORWARD_VECTOR(v);
			ENTITY::SET_ENTITY_VELOCITY(v, vel.x + fwd.x * add, vel.y + fwd.y * add, vel.z + fwd.z * add);
		}
		else
			g_expected = speed; // the engine is ahead of the curve (high gear at low power, downhill): follow it
	}
}

extern "C" __declspec(dllexport) int MLOnLoad(const MLApi* api, const MLContext* ctx)
{
	ml::Init(api, ctx);
	g_enabled = ml::AddToggle("acceleration", "實車加速（0–60 mph：Performance 1.9 秒、Thrust 0.81 秒、Rocket 0.59 秒）", true);
	g_showTimes = ml::AddToggle("showTimes", "顯示 0–96 km/h 成績", true);
	return 1;
}

extern "C" __declspec(dllexport) void MLMain()
{
	for (;;)
	{
		Tick();
		ml::Wait(0);
	}
}
