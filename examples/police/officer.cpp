// The officer's ped and the switches between them and the protagonists.
//
// To the officer: the game's switch, START_PLAYER_SWITCH from the player's ped to the officer (sky camera), control
// moving with CHANGE_PLAYER_PED once the camera is up (switch state 8). The protagonist left behind goes on with their
// day (wanders off), kept by the mod: the game only saves with a protagonist as the player, so saving while the officer
// is played borrows them (see SaveForOfficer).
// Back to a protagonist: the game's own switch request (global 99964: +45 character, +46 0, +44 1; what the selector
// does), so player_controller_b picks the protagonist's scene and creates or reuses their ped. With the officer as the
// player the current character is 145 (none), and the game skips storing the leaving character. The kept protagonist is
// let go first when they are the one switched to, so the game makes them anew in their scene.
#include "police.hpp"

#include <atomic>
#include <cmath>

namespace police::officer
{
	namespace
	{
		::Ped g_officer = 0;
		bool g_uniform = false; // the officer wears the uniform
		Place g_scene{};                  // where the officer was put when created
		const char* g_scenario = nullptr; // what they are busy with there
		bool g_busy = false; // the player (the officer) is still busy with the switch scene's activity
		::Ped g_protagonist = 0; // the protagonist left behind while the officer is played
		int g_headshot = 0;
		constexpr uint32_t kSwitchRequest = 99964;
		constexpr uint32_t kCurrentCharacter = 122220; // 0..2 a protagonist, 145 none

		bool Exists(::Ped p)
		{
			return p && ENTITY::DOES_ENTITY_EXIST(p);
		}

		// Holds a ped nobody controls where it stands.
		void Hold(::Ped p, bool hold)
		{
			ENTITY::SET_ENTITY_AS_MISSION_ENTITY(p, true, true);
			ENTITY::FREEZE_ENTITY_POSITION(p, hold);
			ENTITY::SET_ENTITY_INVINCIBLE(p, hold, false);
			PED::SET_BLOCKING_OF_NON_TEMPORARY_EVENTS(p, hold);
			if (hold)
				TASK::CLEAR_PED_TASKS_IMMEDIATELY(p);
		}

		// Keeps a protagonist nobody plays: they walk about, and nothing kills them while the mod needs them.
		void KeepProtagonist(::Ped p)
		{
			g_protagonist = p;
			ENTITY::SET_ENTITY_AS_MISSION_ENTITY(p, true, true);
			ENTITY::FREEZE_ENTITY_POSITION(p, false);
			ENTITY::SET_ENTITY_INVINCIBLE(p, true, false);
			TASK::TASK_WANDER_STANDARD(p, 10.0f, 10);
		}

		void LetGoOfProtagonist()
		{
			if (Exists(g_protagonist))
			{
				ENTITY::SET_ENTITY_INVINCIBLE(g_protagonist, false, false);
				ENTITY::SET_ENTITY_AS_MISSION_ENTITY(g_protagonist, true, true);
				PED::DELETE_PED(&g_protagonist);
			}
			g_protagonist = 0;
		}

		// The officer as made in the creator (look.cpp).
		::Ped Create(Vector3 at, float heading, bool uniform)
		{
			const Hash model = look::Model();
			const auto loaded = ml::LoadModel(model);
			if (!loaded)
				return 0;
			const ::Ped ped = PED::CREATE_PED(4, model, at.x, at.y, at.z, heading, false, false);
			if (!ped)
				return 0;
			g_uniform = uniform;
			look::Apply(ped, uniform);
			duty::Equip(ped, uniform);
			Hold(ped, true);
			return ped;
		}

		// The game's character number of a wheel slot (0 Michael, 1 Franklin, 2 Trevor).
		int CharacterOf(int slot)
		{
			switch (slot)
			{
			case kSlotMichael: return 0;
			case kSlotFranklin: return 1;
			case kSlotTrevor: return 2;
			default: return -1;
			}
		}
		int CharacterOfModel(Hash model)
		{
			if (model == MISC::GET_HASH_KEY("player_zero"))
				return 0;
			if (model == MISC::GET_HASH_KEY("player_one"))
				return 1;
			if (model == MISC::GET_HASH_KEY("player_two"))
				return 2;
			return -1;
		}

		int64_t GetGlobal(uint32_t index)
		{
			const int64_t* g = ml::scripts::Global(index);
			return g ? static_cast<int32_t>(*g) : -1;
		}
		void SetGlobal(uint32_t index, int64_t value)
		{
			if (int64_t* g = ml::scripts::Global(index))
				*g = value;
		}

		// The game's switch from the player's ped to `to`, busy with `scenario` (or nothing) when the camera comes down;
		// false when it did not complete.
		bool Switch(::Ped to, const char* scenario = nullptr)
		{
			const ::Ped from = PLAYER::PLAYER_PED_ID();
			Hold(to, false);
			if (scenario)
				TASK::TASK_START_SCENARIO_IN_PLACE(to, scenario, 0, true);
			const Vector3 a = ENTITY::GET_ENTITY_COORDS(from, true), b = ENTITY::GET_ENTITY_COORDS(to, true);
			STREAMING::START_PLAYER_SWITCH(from, to, 0, STREAMING::GET_IDEAL_PLAYER_SWITCH_TYPE(a.x, a.y, a.z, b.x, b.y, b.z));
			bool changed = false;
			for (const uint64_t end = ml::TickMs() + 60000; ml::TickMs() < end;)
			{
				ml::Wait(0);
				if (!changed && (STREAMING::GET_PLAYER_SWITCH_STATE() >= 8 || !STREAMING::IS_PLAYER_SWITCH_IN_PROGRESS()))
				{
					PLAYER::CHANGE_PLAYER_PED(PLAYER::PLAYER_ID(), to, false, false);
					if (scenario)
					{
						TASK::TASK_START_SCENARIO_IN_PLACE(to, scenario, 0, true);
						g_busy = true;
					}
					changed = true;
				}
				if (changed && !STREAMING::IS_PLAYER_SWITCH_IN_PROGRESS())
					return true;
			}
			if (!changed)
				STREAMING::STOP_PLAYER_SWITCH();
			return changed;
		}

		void Fade(bool out)
		{
			if (out)
			{
				CAMERA::DO_SCREEN_FADE_OUT(500);
				for (const uint64_t end = ml::TickMs() + 3000; ml::TickMs() < end && !CAMERA::IS_SCREEN_FADED_OUT();)
					ml::Wait(0);
			}
			else
				CAMERA::DO_SCREEN_FADE_IN(500);
		}

		std::atomic<bool> g_saved = false;

		// The game's save screen (the one a safehouse bed opens; autosave may be off in the settings): the player picks
		// a slot or backs out. Scripts, the mod's included, do not run while it is up, so the next frames are after it
		// closed. True when the game wrote a save (the save files are looked at once a second).
		bool SaveScreen()
		{
			g_saved = false;
			MISC::SET_SAVE_MENU_ACTIVE(true);
			ml::Wait(0); // the screen opens
			ml::Wait(0); // ... and these frames run once it has closed
			for (const uint64_t end = ml::TickMs() + 3000; ml::TickMs() < end && !g_saved;)
				ml::Wait(0);
			return g_saved;
		}
	}

	::Ped Ped()
	{
		return Exists(g_officer) ? g_officer : 0;
	}

	bool IsPlayer()
	{
		return Exists(g_officer) && PLAYER::PLAYER_PED_ID() == g_officer;
	}

	const char* Headshot()
	{
		return g_headshot && PED::IS_PEDHEADSHOT_READY(g_headshot) && PED::IS_PEDHEADSHOT_VALID(g_headshot) ? PED::GET_PEDHEADSHOT_TXD_STRING(g_headshot) : nullptr;
	}

	void Ensure()
	{
		if (!Exists(g_officer))
		{
			// Where the hour puts them (the scene's activity starts when they are switched to).
			const life::Scene scene = life::Now();
			g_scene = life::Pick(scene, g_scenario);
			g_officer = Create({g_scene.x, g_scene.y, g_scene.z}, g_scene.heading, scene == life::Scene::OnDuty);
			if (g_headshot)
				PED::UNREGISTER_PEDHEADSHOT(g_headshot);
			g_headshot = 0;
		}
		if (Exists(g_officer) && !g_headshot)
			g_headshot = PED::REGISTER_PEDHEADSHOT(g_officer);
	}

	bool InUniform()
	{
		return g_uniform;
	}

	void Rebuild()
	{
		if (!Exists(g_officer))
			return;
		// A new ped (the model changes with the sex) in the old one's place, in control when the old one was.
		const bool player = IsPlayer();
		const Vector3 at = ENTITY::GET_ENTITY_COORDS(g_officer, true);
		const float heading = ENTITY::GET_ENTITY_HEADING(g_officer);
		const ::Ped ped = Create({at.x, at.y, at.z - 1.0f}, heading, g_uniform);
		if (!ped)
			return;
		::Ped old = g_officer;
		g_officer = ped;
		Hold(ped, !player);
		if (player)
			PLAYER::CHANGE_PLAYER_PED(PLAYER::PLAYER_ID(), ped, false, false);
		ENTITY::SET_ENTITY_AS_MISSION_ENTITY(old, true, true);
		PED::DELETE_PED(&old);
		if (g_headshot)
			PED::UNREGISTER_PEDHEADSHOT(g_headshot);
		g_headshot = PED::REGISTER_PEDHEADSHOT(ped);
	}

	void Dress(bool uniform)
	{
		if (!Exists(g_officer))
			return;
		g_uniform = uniform;
		look::Apply(g_officer, uniform);
		duty::Equip(g_officer, uniform);
		RefreshHeadshot();
	}

	void RefreshHeadshot()
	{
		if (!Exists(g_officer))
			return;
		if (g_headshot)
			PED::UNREGISTER_PEDHEADSHOT(g_headshot);
		g_headshot = PED::REGISTER_PEDHEADSHOT(g_officer);
	}

	void Drop()
	{
		if (!Exists(g_officer) || IsPlayer())
			return;
		if (g_headshot)
			PED::UNREGISTER_PEDHEADSHOT(g_headshot);
		g_headshot = 0;
		ENTITY::SET_ENTITY_AS_MISSION_ENTITY(g_officer, true, true);
		PED::DELETE_PED(&g_officer);
		g_officer = 0;
	}

	void Forget()
	{
		duty::Forget();
		units::Forget();
		menu::Close();
		g_officer = 0;
		g_protagonist = 0;
		g_headshot = 0;
	}

	void SwitchTo()
	{
		Ensure();
		if (!Exists(g_officer) || IsPlayer())
			return;
		const ::Ped from = PLAYER::PLAYER_PED_ID();
		// The scene's place loads first, so it is there when the camera comes down.
		life::Load(g_scene);
		ENTITY::SET_ENTITY_COORDS(g_officer, g_scene.x, g_scene.y, g_scene.z, false, false, false, false);
		ENTITY::SET_ENTITY_HEADING(g_officer, g_scene.heading);
		if (Switch(g_officer, g_scenario))
		{
			KeepProtagonist(from);
			if (!look::Exists())
				ml::Notify("第一次當警察：按 F4 →「Police」→「外觀（角色建立）」建立你的警察");
			ml::Log("switched to the officer from protagonist {:08X}", ENTITY::GET_ENTITY_MODEL(from));
		}
	}

	void SwitchFrom(int slot)
	{
		const int character = CharacterOf(slot);
		if (character < 0 || !IsPlayer())
			return;
		if (Exists(g_protagonist) && CharacterOfModel(ENTITY::GET_ENTITY_MODEL(g_protagonist)) == character)
			LetGoOfProtagonist();
		SetGlobal(kSwitchRequest + 45, character);
		SetGlobal(kSwitchRequest + 46, 0);
		SetGlobal(kSwitchRequest + 44, 1);
		ml::Log("switch to character {} requested from the game", character);
		// The game takes over; once the player is someone else, the officer stays where they were.
		for (const uint64_t end = ml::TickMs() + 60000; ml::TickMs() < end && PLAYER::PLAYER_PED_ID() == g_officer;)
			ml::Wait(0);
		if (PLAYER::PLAYER_PED_ID() == g_officer)
		{
			ml::LogError("the game did not switch to character {}", character);
			return;
		}
		for (const uint64_t end = ml::TickMs() + 60000; ml::TickMs() < end && STREAMING::IS_PLAYER_SWITCH_IN_PROGRESS();)
			ml::Wait(0);
		Drop();
		// A protagonist other than the one switched to stays the mod's no longer: the game looks after them now.
		if (Exists(g_protagonist))
		{
			ENTITY::SET_ENTITY_INVINCIBLE(g_protagonist, false, false);
			ENTITY::SET_PED_AS_NO_LONGER_NEEDED(&g_protagonist);
			g_protagonist = 0;
		}
		ml::Log("switched to character {}", character);
	}

	void Restore(Vector3 at, float heading)
	{
		Ensure();
		if (!Exists(g_officer))
			return;
		const ::Ped from = PLAYER::PLAYER_PED_ID();
		if (life::InApartment(at))
			life::Load({at.x, at.y, at.z, heading});
		ENTITY::SET_ENTITY_COORDS_NO_OFFSET(g_officer, at.x, at.y, at.z, false, false, false);
		ENTITY::SET_ENTITY_HEADING(g_officer, heading);
		Hold(g_officer, false);
		PLAYER::CHANGE_PLAYER_PED(PLAYER::PLAYER_ID(), g_officer, false, false);
		KeepProtagonist(from);
		ml::Log("restored the officer at ({:.1f}, {:.1f}, {:.1f}); protagonist {:08X} kept", at.x, at.y, at.z, ENTITY::GET_ENTITY_MODEL(from));
	}

	void Tick()
	{
		// The scene's activity lasts until the player moves (as after the game's own switches).
		if (g_busy && IsPlayer() && (std::abs(PAD::GET_CONTROL_NORMAL(0, 30)) > 0.2f || std::abs(PAD::GET_CONTROL_NORMAL(0, 31)) > 0.2f))
		{
			TASK::CLEAR_PED_TASKS(g_officer);
			g_busy = false;
		}
		if (!IsPlayer())
			g_busy = false;
	}

	void NoteSaved()
	{
		g_saved = true;
	}

	bool SaveForOfficer()
	{
		if (!IsPlayer())
		{
			// A protagonist saves the game's own way.
			const bool saved = SaveScreen();
			ml::Log("save as a protagonist: saved {}", saved);
			return saved;
		}
		if (!Exists(g_protagonist) || ENTITY::IS_ENTITY_DEAD(g_protagonist, false))
		{
			ml::Notify("無法存檔：找不到留下的主角");
			ml::LogError("save for the officer: no protagonist to save with");
			return false;
		}
		persist::Update(true);
		Fade(true);
		// The protagonist stands still where they are while the game saves with them as the player.
		TASK::CLEAR_PED_TASKS_IMMEDIATELY(g_protagonist);
		ENTITY::FREEZE_ENTITY_POSITION(g_protagonist, true);
		Hold(g_officer, true);
		PLAYER::CHANGE_PLAYER_PED(PLAYER::PLAYER_ID(), g_protagonist, false, false);
		for (const uint64_t end = ml::TickMs() + 5000; ml::TickMs() < end && !(GetGlobal(kCurrentCharacter) >= 0 && GetGlobal(kCurrentCharacter) < 3);)
			ml::Wait(0);
		const int64_t character = GetGlobal(kCurrentCharacter);
		const bool saved = SaveScreen();
		// Back to the officer.
		Hold(g_officer, false);
		PLAYER::CHANGE_PLAYER_PED(PLAYER::PLAYER_ID(), g_officer, false, false);
		KeepProtagonist(g_protagonist);
		Fade(false);
		ml::Log("save for the officer: saved {}, character during the save {}", saved, character);
		return saved;
	}
}
