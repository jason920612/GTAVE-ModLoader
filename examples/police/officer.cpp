// The officer's ped and the switches between them and the protagonists.
//
// To the officer: the game's switch, START_PLAYER_SWITCH from the player's ped to the officer (sky camera), control
// moving with CHANGE_PLAYER_PED once the camera is up (switch state 8). The protagonist left behind is handed back to
// the game as an ordinary ped that walks off, the way a protagonist does after a switch.
// Back to a protagonist: the game's own switch request (global 99964: +45 character, +46 0, +44 1; what the selector
// does), so player_controller_b picks the protagonist's scene and creates or reuses their ped. With the officer as the
// player the current character is 145 (none), and the game skips storing the leaving character.
#include "police.hpp"

namespace police::officer
{
	namespace
	{
		::Ped g_officer = 0;
		int g_headshot = 0;
		constexpr uint32_t kSwitchRequest = 99964;

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

		// Prototype look: a default face in the LSPD summer uniform, at the Mission Row station.
		::Ped Create()
		{
			const Hash model = MISC::GET_HASH_KEY("mp_m_freemode_01");
			const auto loaded = ml::LoadModel(model);
			if (!loaded)
				return 0;
			const ::Ped ped = PED::CREATE_PED(4, model, 432.0f, -981.0f, 30.7f, 90.0f, false, false);
			if (!ped)
				return 0;
			PED::SET_PED_DEFAULT_COMPONENT_VARIATION(ped);
			PED::SET_PED_HEAD_BLEND_DATA(ped, 0, 0, 0, 0, 0, 0, 0.5f, 0.5f, 0.0f, false);
			PED::SET_PED_COMPONENT_VARIATION(ped, 3, 0, 0, 0);   // arms
			PED::SET_PED_COMPONENT_VARIATION(ped, 4, 35, 0, 0);  // trousers
			PED::SET_PED_COMPONENT_VARIATION(ped, 6, 25, 0, 0);  // shoes
			PED::SET_PED_COMPONENT_VARIATION(ped, 8, 58, 0, 0);  // undershirt and belt
			PED::SET_PED_COMPONENT_VARIATION(ped, 11, 55, 0, 0); // shirt
			PED::SET_PED_PROP_INDEX(ped, 0, 46, 0, true, 0);     // cap
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

		void SetGlobal(uint32_t index, int64_t value)
		{
			if (int64_t* g = ml::scripts::Global(index))
				*g = value;
		}

		// The game's switch from the player's ped to `to`; false when it did not complete.
		bool Switch(::Ped to)
		{
			const ::Ped from = PLAYER::PLAYER_PED_ID();
			Hold(to, false);
			const Vector3 a = ENTITY::GET_ENTITY_COORDS(from, true), b = ENTITY::GET_ENTITY_COORDS(to, true);
			STREAMING::START_PLAYER_SWITCH(from, to, 0, STREAMING::GET_IDEAL_PLAYER_SWITCH_TYPE(a.x, a.y, a.z, b.x, b.y, b.z));
			bool changed = false;
			for (const uint64_t end = ml::TickMs() + 60000; ml::TickMs() < end;)
			{
				ml::Wait(0);
				if (!changed && (STREAMING::GET_PLAYER_SWITCH_STATE() >= 8 || !STREAMING::IS_PLAYER_SWITCH_IN_PROGRESS()))
				{
					PLAYER::CHANGE_PLAYER_PED(PLAYER::PLAYER_ID(), to, false, false);
					changed = true;
				}
				if (changed && !STREAMING::IS_PLAYER_SWITCH_IN_PROGRESS())
					return true;
			}
			if (!changed)
				STREAMING::STOP_PLAYER_SWITCH();
			return changed;
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
			g_officer = Create();
			if (g_headshot)
				PED::UNREGISTER_PEDHEADSHOT(g_headshot);
			g_headshot = 0;
		}
		if (Exists(g_officer) && !g_headshot)
			g_headshot = PED::REGISTER_PEDHEADSHOT(g_officer);
	}

	void SwitchTo()
	{
		Ensure();
		if (!Exists(g_officer) || IsPlayer())
			return;
		::Ped from = PLAYER::PLAYER_PED_ID();
		if (Switch(g_officer))
		{
			// The protagonist goes on with their day.
			TASK::TASK_WANDER_STANDARD(from, 10.0f, 10);
			ml::Log("switched to the officer from protagonist {:08X}", ENTITY::GET_ENTITY_MODEL(from));
		}
	}

	void SwitchFrom(int slot)
	{
		const int character = CharacterOf(slot);
		if (character < 0 || !IsPlayer())
			return;
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
		Hold(g_officer, true);
		ml::Log("switched to character {}", character);
	}
}
