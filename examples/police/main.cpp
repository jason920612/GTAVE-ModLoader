// Police: a fourth character, a police officer with a life of their own off duty (design and research: research/phase0.md §53).
// Stage 1: the officer on the character wheel, switching both ways, kept in the game's save.
#include "police.hpp"

#include <atomic>

ML_MOD_INFO("Police", "0.0.1", "ModLoader", "Play as a police officer (in development)")

namespace
{
	std::atomic<bool> g_saveRequested = false;
	std::atomic<bool> g_menuRequested = false;
}

extern "C" __declspec(dllexport) int MLOnLoad(const MLApi* api, const MLContext* ctx)
{
	ml::Init(api, ctx);
	ml::game::On(ml::game::Event::Saved, [] { police::officer::NoteSaved(); });
	ml::game::On(ml::game::Event::SaveLoading, [] { police::persist::Loading(); });
	// The menu's callback shares a fiber with the game events the save waits for: the main loop does the saving.
	ml::Root().Action("存檔", [] { g_saveRequested = true; });
	police::look::BuildMenu();
	ml::Hotkey("hkPoliceMenu", "警察選單", 0x78 /* F9 (F6-F8 are the trainer's) */, [] { g_menuRequested = true; });
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
		police::duty::Tick();
		police::units::Tick();
		police::look::Tick();
		if (g_menuRequested)
		{
			g_menuRequested = false;
			if (police::menu::IsOpen())
				police::menu::Close();
			else if (!police::officer::IsPlayer())
				ml::Notify("警察選單只在當警察時能用");
			else if (!police::duty::OnDuty())
				ml::Notify("要先上班（到警局換上制服）");
			else
				police::menu::Open(police::units::Menu());
		}
		if (!police::officer::IsPlayer())
			police::menu::Close();
		police::menu::Tick();
		if (g_saveRequested)
		{
			police::officer::SaveForOfficer();
			g_saveRequested = false; // presses during the save are the same save
		}
	}
}
