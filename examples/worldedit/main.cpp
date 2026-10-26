// World editor: draw roads like a city builder, over the land the world builder made (tools/worldgen).
#include <Windows.h>

#include <modloader/natives.hpp>

#include "editor.hpp"

ML_MOD_INFO("World Editor", "0.1.0", "ModLoader", "像城市建造遊戲一樣畫道路（F7）")

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
