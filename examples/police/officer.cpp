// The officer's ped and the switches between them and the protagonists.
//
// A switch is the game's own: START_PLAYER_SWITCH from the player's ped to the target ped (sky camera), and control
// moves to the target with CHANGE_PLAYER_PED once the camera is up (switch state 8). The ped the player leaves stays
// in the world where it was, held in place (frozen, invincible, ignoring events) until it is switched back to.
#include "police.hpp"

namespace police::officer
{
	namespace
	{
		::Ped g_officer = 0;
		::Ped g_parked = 0; // the protagonist left behind while the officer is played
		int g_headshot = 0;

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

		Hash ModelOf(int slot)
		{
			switch (slot)
			{
			case kSlotMichael: return MISC::GET_HASH_KEY("player_zero");
			case kSlotFranklin: return MISC::GET_HASH_KEY("player_one");
			case kSlotTrevor: return MISC::GET_HASH_KEY("player_two");
			default: return 0;
			}
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
					Hold(from, true);
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
		const ::Ped from = PLAYER::PLAYER_PED_ID();
		if (Switch(g_officer))
		{
			g_parked = from;
			ml::Log("switched to the officer; protagonist {:08X} held at their place", ENTITY::GET_ENTITY_MODEL(from));
		}
	}

	void SwitchFrom(int slot)
	{
		const Hash model = ModelOf(slot);
		if (!model || !IsPlayer())
			return;
		if (!Exists(g_parked) || ENTITY::GET_ENTITY_MODEL(g_parked) != model)
		{
			ml::Notify("（開發中）目前只能切回剛才的主角");
			ml::Log("switch to slot {}: that protagonist is not the one left behind (prototype)", slot);
			return;
		}
		if (Switch(g_parked))
		{
			g_parked = 0;
			ml::Log("switched back to protagonist {:08X}", model);
		}
	}
}
