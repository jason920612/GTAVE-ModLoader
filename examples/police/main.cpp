// Police: a fourth character, a police officer with a life of their own off duty (design and research: research/phase0.md §53).
// Stage 1: the officer on the character wheel, switching both ways, kept in the game's save.
#include "police.hpp"

#include <atomic>

ML_MOD_INFO("Police", "0.0.1", "ModLoader", "Play as a police officer (in development)")

namespace
{
	std::atomic<bool> g_saveRequested = false;
}

extern "C" __declspec(dllexport) int MLOnLoad(const MLApi* api, const MLContext* ctx)
{
	ml::Init(api, ctx);
	ml::game::On(ml::game::Event::Saved, [] { police::officer::NoteSaved(); });
	ml::game::On(ml::game::Event::SaveLoading, [] { police::persist::Loading(); });
	// The menu's callback shares a fiber with the game events the save waits for: the main loop does the saving.
	ml::Root().Action("存檔", [] { g_saveRequested = true; });
	police::look::BuildMenu();
	return 1;
}

extern "C" __declspec(dllexport) void MLMain()
{
	for (;;)
	{
		ml::Wait(0);
		police::persist::Tick();
		police::wheel::Tick();
		police::officer::Tick();
		police::life::Tick();
		police::look::Tick();
		if (g_saveRequested)
		{
			police::officer::SaveForOfficer();
			g_saveRequested = false; // presses during the save are the same save
		}
	}
}
