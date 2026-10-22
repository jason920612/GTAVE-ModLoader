// The character wheel.
//
// The selector script draws the wheel on a PLAYER_SWITCH scaleform movie (its static 207) and keeps its state in
// global 24447: +1 wheel open, +69 hovered slot, +20..+23 slot states (1 selectable), +25..+28 slot types. Its layout
// statics 199..204 place the movie (right/bottom aligned). While a protagonist is played the mod draws the officer into
// the bottom slot (GTA Online's) and acts when the wheel closes on it. Signed in, story mode leaves that slot empty and
// never acts on it; in the launcher's offline mode the selector offers it (state 1, type 3) and, on release, starts its
// GTA Online path, which shows "sign in to play GTA Online". So for the selector only, the wheel key's release is not
// reported while the officer's slot is hovered (Init), and the selector closes the wheel without acting.
//
// The script refuses to open the wheel for anyone but the three protagonists (it reads the player's model), so while
// the officer is played the mod runs its own wheel: its own instance of the same movie, the same layout, the same
// stick/mouse direction to slot rule (angle from straight up; slots centred on 0, 90, 180, 270 degrees).
#include "police.hpp"

#include <cmath>
#include <cstring>

namespace police::wheel
{
	namespace
	{
		constexpr uint32_t kSelector = 24447;
		constexpr int kWheelControl = 19; // INPUT_CHARACTER_WHEEL

		int32_t Selector(int offset)
		{
			const int64_t* g = ml::scripts::Global(kSelector + offset);
			return g ? static_cast<int32_t>(*g) : 0;
		}

		int32_t g_selectorThread = -1;
		const int64_t* SelectorStatic(uint32_t index)
		{
			if (g_selectorThread < 0)
				for (const auto& t : ml::scripts::Threads())
					if (t.name == "selector")
						g_selectorThread = t.id;
			const int64_t* s = g_selectorThread >= 0 ? ml::scripts::Static(g_selectorThread, index) : nullptr;
			if (!s)
				g_selectorThread = -1;
			return s;
		}
		float SelectorFloat(uint32_t index)
		{
			const int64_t* s = SelectorStatic(index);
			float f = 0;
			if (s)
				std::memcpy(&f, s, sizeof f);
			return f;
		}

		void Call(int movie, const char* method, std::initializer_list<int> ints, const char* text = nullptr)
		{
			if (!GRAPHICS::BEGIN_SCALEFORM_MOVIE_METHOD(movie, method))
				return;
			for (const int i : ints)
				GRAPHICS::SCALEFORM_MOVIE_METHOD_ADD_PARAM_INT(i);
			if (text)
				GRAPHICS::SCALEFORM_MOVIE_METHOD_ADD_PARAM_LITERAL_STRING(text);
			GRAPHICS::END_SCALEFORM_MOVIE_METHOD();
		}

		void SetSlot(int movie, int slot, int state, int type)
		{
			if (!GRAPHICS::BEGIN_SCALEFORM_MOVIE_METHOD(movie, "SET_SWITCH_SLOT"))
				return;
			GRAPHICS::SCALEFORM_MOVIE_METHOD_ADD_PARAM_INT(slot);
			GRAPHICS::SCALEFORM_MOVIE_METHOD_ADD_PARAM_INT(state);
			GRAPHICS::SCALEFORM_MOVIE_METHOD_ADD_PARAM_INT(type);
			GRAPHICS::SCALEFORM_MOVIE_METHOD_ADD_PARAM_BOOL(false);
			GRAPHICS::END_SCALEFORM_MOVIE_METHOD();
		}

		void ShowOfficer(int movie)
		{
			SetSlot(movie, kSlotOfficer, 1, Selector(25 + kSlotOfficer));
			if (const char* head = officer::Headshot())
				Call(movie, "SET_MULTIPLAYER_HEAD", {}, head);
			Call(movie, "SET_MP_LABEL", {}, "警察");
		}

		// ---- while a protagonist is played: the game's wheel ------------------------------------------

		bool g_wasOpen = false;
		int g_lastHover = -1;

		void GameWheel()
		{
			const bool open = Selector(1) != 0;
			const int hover = Selector(69);
			if (open)
			{
				officer::Ensure();
				if (const int64_t* movie = SelectorStatic(207); movie && *movie)
				{
					ShowOfficer(static_cast<int>(*movie));
					if (hover == kSlotOfficer)
						Call(static_cast<int>(*movie), "SET_PLAYER_SELECTED", {kSlotOfficer});
				}
			}
			else if (g_wasOpen && g_lastHover == kSlotOfficer)
				officer::SwitchTo();
			else if (PAD::IS_CONTROL_JUST_RELEASED(0, 169) && !STREAMING::IS_PLAYER_SWITCH_IN_PROGRESS())
			{
				// F8, the bottom slot's key (hidden from the selector, see Init).
				if (officer::WhyNoSwitch(true))
					ml::Notify("現在不能切換到警察");
				officer::SwitchTo();
			}
			else if (g_wasOpen)
				officer::Drop(); // the wheel closed on someone else: the officer is not needed here
			g_wasOpen = open;
			g_lastHover = open ? hover : -1;
		}

		// ---- while the officer is played: the mod's own wheel ----------------------------------------

		int g_movie = 0;
		bool g_open = false;
		int g_hover = kSlotOfficer;

		void Draw()
		{
			if (GRAPHICS::GET_IS_WIDESCREEN())
				HUD::SET_WIDESCREEN_FORMAT(3);
			GRAPHICS::SET_SCRIPT_GFX_ALIGN('R', 'B');
			GRAPHICS::SET_SCRIPT_GFX_ALIGN_PARAMS(0, 0, 0, 0);
			const float w = SelectorFloat(200) * SelectorFloat(202) * SelectorFloat(204);
			GRAPHICS::DRAW_SCALEFORM_MOVIE(g_movie, w * 0.5f, SelectorFloat(199), w, SelectorFloat(201) * SelectorFloat(203), 255, 255, 255, 255, 0);
			GRAPHICS::RESET_SCRIPT_GFX_ALIGN();
		}

		// Slot under the stick / mouse (-1 = no clear direction).
		int Pointed()
		{
			const float x = PAD::GET_DISABLED_CONTROL_NORMAL(0, 13), y = PAD::GET_DISABLED_CONTROL_NORMAL(0, 12);
			if (std::sqrt(x * x + y * y) <= 0.3f)
				return -1;
			float angle = MISC::GET_ANGLE_BETWEEN_2D_VECTORS(0.0f, -1.0f, x, y);
			if (x < 0)
				angle = 360.0f - angle;
			return static_cast<int>(std::fmod(angle + 45.0f, 360.0f) / 90.0f) % 4;
		}

		void OwnWheel()
		{
			const bool held = PAD::IS_DISABLED_CONTROL_PRESSED(0, kWheelControl) || PAD::IS_CONTROL_PRESSED(0, kWheelControl);
			if (!g_movie)
				g_movie = GRAPHICS::REQUEST_SCALEFORM_MOVIE_INSTANCE("PLAYER_SWITCH");
			if (held && !g_open && GRAPHICS::HAS_SCALEFORM_MOVIE_LOADED(g_movie) && !officer::WhyNoSwitch(false))
			{
				g_open = true;
				g_hover = kSlotOfficer;
				for (const int slot : {kSlotFranklin, kSlotTrevor, kSlotMichael})
					SetSlot(g_movie, slot, 1, Selector(25 + slot));
				ShowOfficer(g_movie);
				Call(g_movie, "SET_SWITCH_VISIBLE", {1});
				Call(g_movie, "SET_PLAYER_SELECTED", {g_hover});
			}
			if (!g_open)
				return;
			if (held)
			{
				PAD::SET_INPUT_EXCLUSIVE(0, 12);
				PAD::SET_INPUT_EXCLUSIVE(0, 13);
				PAD::DISABLE_CONTROL_ACTION(0, 1, true); // look
				PAD::DISABLE_CONTROL_ACTION(0, 2, true);
				if (const int slot = Pointed(); slot >= 0 && slot != g_hover)
				{
					g_hover = slot;
					Call(g_movie, "SET_PLAYER_SELECTED", {g_hover});
				}
				HUD::HIDE_HUD_COMPONENT_THIS_FRAME(19); // weapon wheel
				Draw();
				return;
			}
			g_open = false;
			Call(g_movie, "SET_SWITCH_VISIBLE", {0});
			if (g_hover != kSlotOfficer)
				officer::SwitchFrom(g_hover);
		}
	}

	void Init()
	{
		constexpr uint64_t kIsControlJustReleased = 0x50F940259D3841E6ULL;
		constexpr uint64_t kIsDisabledControlJustReleased = 0x305C8DCD79DA8B0FULL;
		constexpr int kSelectMultiplayer = 169; // F8: the bottom slot directly
		for (const uint64_t hash : {kIsControlJustReleased, kIsDisabledControlJustReleased})
			ml::scripts::OverrideNative("selector", hash, [](ml::scripts::NativeCall& call) {
				const int action = call.Arg<int32_t>(1);
				const bool onOfficer = Selector(1) != 0 && Selector(69) == kSlotOfficer;
				if ((action == kWheelControl && onOfficer) || action == kSelectMultiplayer)
					call.Return<int32_t>(0);
				else
					call.CallOriginal();
			});
	}

	void Tick()
	{
		if (officer::IsPlayer())
		{
			g_wasOpen = false;
			OwnWheel();
		}
		else
		{
			g_open = false;
			GameWheel();
		}
	}
}
