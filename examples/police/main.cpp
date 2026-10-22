// Police: a fourth character, a police officer with a life of their own off duty (design and research: research/phase0.md §53).
// Stage 1: the officer on the character wheel, switching both ways, kept in the game's save.
#include "police.hpp"

#include <atomic>

ML_MOD_INFO("Police", "0.0.1", "ModLoader", "Play as a police officer (in development)")

namespace
{
	std::atomic<bool> g_saveRequested = false;
	std::atomic<bool> g_menuRequested = false;
	std::atomic<int> g_testSpawn = -1; // development: a test suspect of this skill (or 9 = clear)
}

extern "C" __declspec(dllexport) int MLOnLoad(const MLApi* api, const MLContext* ctx)
{
	ml::Init(api, ctx);
	ml::game::On(ml::game::Event::Saved, [] { police::officer::NoteSaved(); });
	ml::game::On(ml::game::Event::SaveLoading, [] { police::persist::Loading(); });
	// The menu's callback shares a fiber with the game events the save waits for: the main loop does the saving.
	ml::Root().Action("存檔", [] { g_saveRequested = true; });
	police::guard::Init();
	police::look::BuildMenu();
	{
		// Development: suspects to try the AI on.
		const ml::Page test = ml::Root().Sub("開發測試（嫌犯 AI）");
		test.Action("生成新手嫌犯（前方 12 公尺）", [] { g_testSpawn = 0; });
		test.Action("生成普通嫌犯", [] { g_testSpawn = 1; });
		test.Action("生成老手嫌犯", [] { g_testSpawn = 2; });
		test.Action("生成會逃跑的老手（高罪責）", [] { g_testSpawn = 3; });
		test.Action("生成開車的嫌犯（前方道路）", [] { g_testSpawn = 4; });
		test.Action("生成開車的老手嫌犯（高罪責）", [] { g_testSpawn = 5; });
		test.Action("生成持槍的兇嫌", [] { g_testSpawn = 6; });
		test.Action("生成會配合的路人", [] { g_testSpawn = 7; });
		test.Action("立即派遣一件報案", [] { police::dispatch::CallNow(); });
		test.Action("立即派遣（嫌犯會配合，測試搜查用）", [] { police::dispatch::CallNow(true); });
		test.Action("清除測試嫌犯", [] { g_testSpawn = 9; });
	}
	police::lights::BuildKeys();
	police::mdt::Build();
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
		police::guard::Tick();
		police::life::Tick();
		police::duty::Tick();
		police::units::Tick();
		police::lights::Tick();
		police::suspect::Tick();
		police::custody::Tick();
		police::ambient::Tick();
		police::dispatch::Tick();
		police::search::Tick();
		police::mdt::Tick();
		if (const int t = g_testSpawn.exchange(-1); t >= 0)
		{
			if (t == 9)
				police::suspect::Clear();
			else
			{
				const ::Ped me = PLAYER::PLAYER_PED_ID();
				const Vector3 at = ENTITY::GET_OFFSET_FROM_ENTITY_IN_WORLD_COORDS(me, 0.0f, 12.0f, 0.0f);
				auto p = police::suspect::RandomProfile(t == 0 ? police::ai::Skill::Novice : t == 1 || t == 4 ? police::ai::Skill::Average : police::ai::Skill::Veteran);
				if (t == 3 || t == 5)
					p.guilt = 0.95f, p.nerve = 0.8f, p.hostility = 0.1f, p.armed = 0;
				if (t == 6)
					p.guilt = 0.9f, p.nerve = 0.9f, p.hostility = 0.95f, p.armed = 2, p.impairment = police::ai::Impairment::Sober;
				if (t == 7)
					p.guilt = 0.1f, p.nerve = 0.2f, p.hostility = 0.05f, p.armed = 0, p.impairment = police::ai::Impairment::Sober;
				if (t == 4 || t == 5)
					police::suspect::SpawnDriver(ENTITY::GET_OFFSET_FROM_ENTITY_IN_WORLD_COORDS(me, 0.0f, 35.0f, 0.0f), p);
				else
					police::suspect::Spawn(at, ENTITY::GET_ENTITY_HEADING(me) + 180.0f, p);
			}
		}
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
