// World editor: roads, buildings, ground and terrain edited like a city builder and a paint program, over the land the
// world builder made (tools/worldgen).
#include <Windows.h>

#include <modloader/natives.hpp>

#include "editor.hpp"

ML_MOD_INFO("World Editor", "0.2.0", "ModLoader", "像城市建造遊戲和繪圖軟體一樣編輯世界（F7）：道路、建築、地面、地形、等高線、氣候")

extern "C" __declspec(dllexport) int MLOnLoad(const MLApi* api, const MLContext* ctx)
{
	ml::Init(api, ctx);
	worldedit::Init();
	return 1;
}

extern "C" __declspec(dllexport) void MLMain()
{
	for (;;)
	{
		worldedit::Frame();
		ml::Wait(0);
	}
}
