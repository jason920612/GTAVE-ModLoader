// Emergency lights in stages, like US police cars (user's design), on every emergency vehicle the player drives and
// on the mod's own police cars (backup, the colleague bringing the patrol car):
//   stage 1  rear warning only (amber, flashing at the back of the light bar), parked at the roadside
//   stage 2  all lights, no siren (a traffic stop, on scene)
//   stage 3  lights and siren (an emergency run)
// Q goes 1 → 2 → 3 → off, R changes the siren's tone, holding the horn (E) sounds the air horn; Q and R can be
// rebound in the loader, the horn is the game's own control. The game's own E-toggles-the-siren is off in these
// vehicles. Tones: the vehicle's own siren, or one of the game's siren sounds played from the vehicle with its own
// siren muted (names checked in game: a sound that does not exist finishes at once, research §53.4).
#include "police.hpp"

#include <atomic>
#include <string>
#include <unordered_map>

namespace police::lights
{
	namespace
	{
		struct Tone
		{
			const char* name;  // shown when changed
			const char* sound; // nullptr = the vehicle's own siren
		};
		const Tone kTones[] = {
		    {"原廠", nullptr},
		    {"音調 A", "VEHICLES_HORNS_SIREN_1"},
		    {"音調 B", "VEHICLES_HORNS_SIREN_2"},
		    {"音調 C", "VEHICLES_HORNS_POLICE_WARNING"},
		};
		constexpr const char* kAirHorn = "SIRENS_AIRHORN";

		struct State
		{
			int stage = 0;
			int tone = 0;
			int sound = -1; // the siren sound playing (a tone other than the vehicle's own)
		};
		std::unordered_map<Vehicle, State> g_states;

		std::atomic<bool> g_stagePressed = false, g_tonePressed = false;
		int g_horn = -1; // the air horn sound while the horn is held
		uint64_t g_showUntil = 0;
		std::string g_shown;

		constexpr int kHornControl = 86;  // INPUT_VEH_HORN
		constexpr int kRadioWheel = 85;   // INPUT_VEH_RADIO_WHEEL (Q by default)
		constexpr int kNextRadio = 81, kPrevRadio = 82;

		bool Exists(Entity e)
		{
			return e && ENTITY::DOES_ENTITY_EXIST(e);
		}

		void StopSound(int& id)
		{
			if (id >= 0)
			{
				AUDIO::STOP_SOUND(id);
				AUDIO::RELEASE_SOUND_ID(id);
			}
			id = -1;
		}

		void Apply(Vehicle v, State& s)
		{
			const bool lights = s.stage >= 2;
			const bool own = kTones[s.tone].sound == nullptr;
			VEHICLE::SET_VEHICLE_HAS_MUTED_SIRENS(v, !(s.stage == 3 && own));
			if ((VEHICLE::IS_VEHICLE_SIREN_ON(v) != 0) != lights)
				VEHICLE::SET_VEHICLE_SIREN(v, lights);
			AUDIO::SET_SIREN_WITH_NO_DRIVER(v, lights);
			const bool play = s.stage == 3 && !own;
			if (!play)
				StopSound(s.sound);
			else if (s.sound < 0)
			{
				s.sound = AUDIO::GET_SOUND_ID();
				AUDIO::PLAY_SOUND_FROM_ENTITY(s.sound, kTones[s.tone].sound, v, nullptr, false, 0);
			}
		}

		// Stage 1: amber lights flashing left and right at the back of the light bar. The game has no such state of its
		// own (the indicators, SET_VEHICLE_INDICATOR_LIGHTS, do not light on these cars), so they are drawn: the two
		// rearmost of the light bar's lamps (bones siren1..siren20), or the back of the roof when there are none.
		void Warning(Vehicle v)
		{
			Vector3 lamps[2]{};
			float back[2] = {1e9f, 1e9f};
			for (int i = 1; i <= 20; ++i)
			{
				const std::string bone = "siren" + std::to_string(i);
				const int b = ENTITY::GET_ENTITY_BONE_INDEX_BY_NAME(v, bone.c_str());
				if (b < 0)
					continue;
				const Vector3 w = ENTITY::GET_WORLD_POSITION_OF_ENTITY_BONE(v, b);
				const Vector3 l = ENTITY::GET_OFFSET_FROM_ENTITY_GIVEN_WORLD_COORDS(v, w.x, w.y, w.z);
				// The rearmost lamp on each side.
				const int side = l.x < 0 ? 0 : 1;
				if (l.y < back[side])
				{
					back[side] = l.y;
					lamps[side] = l;
				}
			}
			for (int side = 0; side < 2; ++side)
				if (back[side] > 1e8f)
				{
					Vector3 mn{}, mx{};
					MISC::GET_MODEL_DIMENSIONS(ENTITY::GET_ENTITY_MODEL(v), &mn, &mx);
					lamps[side] = {side ? 0.45f : -0.45f, mn.y * 0.35f, mx.z};
				}
			const int phase = static_cast<int>(ml::TickMs() / 250) % 2;
			const Vector3 at = lamps[phase];
			const Vector3 w = ENTITY::GET_OFFSET_FROM_ENTITY_IN_WORLD_COORDS(v, at.x, at.y - 0.05f, at.z - 0.03f);
			GRAPHICS::DRAW_LIGHT_WITH_RANGE(w.x, w.y, w.z, 255, 140, 0, 2.5f, 12.0f);
			GRAPHICS::DRAW_MARKER(28, w.x, w.y, w.z, 0, 0, 0, 0, 0, 0, 0.045f, 0.045f, 0.035f, 255, 150, 20, 255, false, false, 2, false, nullptr,
			    nullptr, false);
		}

		void Show(const std::string& text)
		{
			g_shown = text;
			g_showUntil = ml::TickMs() + 2000;
		}

		std::string StageText(int stage)
		{
			switch (stage)
			{
			case 1: return "警燈 第 1 段：後方警示燈";
			case 2: return "警燈 第 2 段：全部警燈";
			case 3: return "警燈 第 3 段：警燈＋警笛";
			default: return "警燈 關";
			}
		}

		// The vehicle the player drives, if it is an emergency vehicle with a siren.
		Vehicle Driven()
		{
			const ::Ped pl = PLAYER::PLAYER_PED_ID();
			if (!PED::IS_PED_IN_ANY_VEHICLE(pl, false))
				return 0;
			const Vehicle v = PED::GET_VEHICLE_PED_IS_IN(pl, false);
			if (VEHICLE::GET_PED_IN_VEHICLE_SEAT(v, -1, false) != pl || VEHICLE::GET_VEHICLE_CLASS(v) != 18)
				return 0;
			return v;
		}
	}

	void Set(Vehicle v, int stage)
	{
		if (!Exists(v))
			return;
		State& s = g_states[v];
		s.stage = stage;
		Apply(v, s);
	}

	int Stage(Vehicle v)
	{
		const auto it = g_states.find(v);
		return it == g_states.end() ? 0 : it->second.stage;
	}

	void Forget()
	{
		g_states.clear();
		g_horn = -1;
	}

	void BuildKeys()
	{
		ml::Hotkey("hkLightStage", "警燈段數（駕駛警車時）", 'Q', [] { g_stagePressed = true; });
		ml::Hotkey("hkSirenTone", "警笛音調（駕駛警車時）", 'R', [] { g_tonePressed = true; });
	}

	void Tick()
	{
		// Vehicles gone: their sounds stop.
		for (auto it = g_states.begin(); it != g_states.end();)
		{
			if (!Exists(it->first) || ENTITY::IS_ENTITY_DEAD(it->first, false))
			{
				StopSound(it->second.sound);
				it = g_states.erase(it);
			}
			else
				++it;
		}

		for (auto& [veh, st] : g_states)
			if (st.stage == 1)
				Warning(veh);

		const Vehicle v = Driven();
		const bool stagePressed = g_stagePressed.exchange(false), tonePressed = g_tonePressed.exchange(false);
		if (v)
		{
			PAD::DISABLE_CONTROL_ACTION(0, kHornControl, true); // the game's siren toggle
			PAD::DISABLE_CONTROL_ACTION(0, kRadioWheel, true);
			PAD::DISABLE_CONTROL_ACTION(0, kNextRadio, true);
			PAD::DISABLE_CONTROL_ACTION(0, kPrevRadio, true);
			State& s = g_states[v];
			// The vehicle came with its lights on (or the game turned them on): that is stage 2 or 3.
			if (s.stage < 2 && VEHICLE::IS_VEHICLE_SIREN_ON(v))
				s.stage = VEHICLE::IS_VEHICLE_SIREN_AUDIO_ON(v) ? 3 : 2;
			if (stagePressed)
			{
				s.stage = (s.stage + 1) % 4;
				Apply(v, s);
				Show(StageText(s.stage));
			}
			if (tonePressed)
			{
				s.tone = (s.tone + 1) % static_cast<int>(std::size(kTones));
				StopSound(s.sound);
				Apply(v, s);
				Show(std::string("警笛音調：") + kTones[s.tone].name);
			}
			// The air horn while the horn is held.
			if (PAD::IS_DISABLED_CONTROL_PRESSED(0, kHornControl))
			{
				if (g_horn < 0)
				{
					g_horn = AUDIO::GET_SOUND_ID();
					AUDIO::PLAY_SOUND_FROM_ENTITY(g_horn, kAirHorn, v, nullptr, false, 0);
				}
			}
			else
				StopSound(g_horn);
		}
		else
			StopSound(g_horn);


		// What was just changed, beside the minimap for a moment.
		if (ml::TickMs() >= g_showUntil)
			return;
		HUD::SET_TEXT_FONT(0);
		HUD::SET_TEXT_SCALE(0.0f, 0.42f);
		HUD::SET_TEXT_COLOUR(255, 255, 255, 255);
		HUD::SET_TEXT_OUTLINE();
		HUD::BEGIN_TEXT_COMMAND_DISPLAY_TEXT("STRING");
		HUD::ADD_TEXT_COMPONENT_SUBSTRING_PLAYER_NAME(g_shown.c_str());
		HUD::END_TEXT_COMMAND_DISPLAY_TEXT(0.17f, 0.93f, 0);
	}
}
