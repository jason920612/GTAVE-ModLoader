// Example mod: logs the player's position every 5 seconds and spawns an Adder with F5.
#include <Windows.h>

#include <modloader/natives.hpp>

ML_MOD_INFO("Hello Mod", "1.0.0", "ModLoader", "Spawns an Adder with F5")

namespace
{
	bool KeyPressed(int vk)
	{
		return (GetAsyncKeyState(vk) & 1) != 0;
	}

	void SpawnAdder()
	{
		const Hash model = MISC::GET_HASH_KEY("adder");
		STREAMING::REQUEST_MODEL(model);
		const auto deadline = ml::TickMs() + 5000;
		while (!STREAMING::HAS_MODEL_LOADED(model))
		{
			if (ml::TickMs() > deadline)
			{
				ml::LogError("model did not load");
				return;
			}
			ml::Wait(0);
		}

		const Ped player = PLAYER::PLAYER_PED_ID();
		const Vector3 pos = ENTITY::GET_OFFSET_FROM_ENTITY_IN_WORLD_COORDS(player, 0.0f, 5.0f, 0.0f);
		const float heading = ENTITY::GET_ENTITY_HEADING(player);
		const Vehicle vehicle = VEHICLE::CREATE_VEHICLE(model, pos.x, pos.y, pos.z, heading, FALSE, FALSE, FALSE);
		STREAMING::SET_MODEL_AS_NO_LONGER_NEEDED(model);
		if (!vehicle)
		{
			ml::LogError("CREATE_VEHICLE failed");
			return;
		}
		PED::SET_PED_INTO_VEHICLE(player, vehicle, -1);
		ml::Log("spawned Adder (handle {}) at {:.1f}, {:.1f}, {:.1f}", vehicle, pos.x, pos.y, pos.z);
	}
}

extern "C" __declspec(dllexport) int MLOnLoad(const MLApi* api, const MLContext* ctx)
{
	ml::Init(api, ctx);
	ml::Log("loaded; data folder is ready");
	return 1;
}

extern "C" __declspec(dllexport) void MLMain()
{
	ml::Log("MLMain started");
	uint64_t nextReport = 0;
	for (;;)
	{
		if (KeyPressed(VK_F5))
			SpawnAdder();

		if (ml::TickMs() >= nextReport)
		{
			nextReport = ml::TickMs() + 5000;
			const Vector3 pos = ENTITY::GET_ENTITY_COORDS(PLAYER::PLAYER_PED_ID(), TRUE);
			ml::Log("player at {:.1f}, {:.1f}, {:.1f}", pos.x, pos.y, pos.z);
		}
		ml::Wait(0);
	}
}
