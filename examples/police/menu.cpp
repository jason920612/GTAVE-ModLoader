// The in-game list menu (user's choice: like GTA Online's interaction menu): drawn top left while the game runs on,
// ↑/↓ or the mouse wheel to move, Enter or a left click to choose, Backspace, Esc or a right click to go back. The
// police menu (F9) and the action menu on a person or vehicle (hold E) both use it.
#include "police.hpp"

#include <algorithm>

namespace police::menu
{
	namespace
	{
		std::vector<List> g_stack; // the open menu and the ones it came from
		size_t g_cursor = 0;
		std::vector<size_t> g_cursors; // the cursor in each menu below the open one
		uint64_t g_openedAt = 0;

		// Layout (screen fractions).
		constexpr float kLeft = 0.015f, kTop = 0.08f, kWidth = 0.23f;
		constexpr float kHeader = 0.045f, kRow = 0.033f;
		constexpr int kVisible = 10;

		// Controls (pad 0).
		constexpr int kUp = 172, kDown = 173, kSelect = 176, kBack = 177;         // INPUT_CELLPHONE_*
		constexpr int kWheelUp = 241, kWheelDown = 242;                           // INPUT_CURSOR_SCROLL_*
		constexpr int kAttack = 24, kAim = 25;

		void Text(const std::string& s, float x, float y, float scale, int r, int g, int b, bool right = false)
		{
			HUD::SET_TEXT_FONT(0);
			HUD::SET_TEXT_SCALE(0.0f, scale);
			HUD::SET_TEXT_COLOUR(r, g, b, 255);
			if (right)
			{
				HUD::SET_TEXT_RIGHT_JUSTIFY(true);
				HUD::SET_TEXT_WRAP(0.0f, x);
			}
			HUD::BEGIN_TEXT_COMMAND_DISPLAY_TEXT("STRING");
			HUD::ADD_TEXT_COMPONENT_SUBSTRING_PLAYER_NAME(s.c_str());
			HUD::END_TEXT_COMMAND_DISPLAY_TEXT(x, y, 0);
		}

		void Rect(float x, float y, float w, float h, int r, int g, int b, int a)
		{
			GRAPHICS::DRAW_RECT(x + w / 2, y + h / 2, w, h, r, g, b, a, false);
		}

		bool Pressed(int control)
		{
			return PAD::IS_DISABLED_CONTROL_JUST_PRESSED(0, control);
		}

		void Hover()
		{
			if (g_stack.empty())
				return;
			const auto& items = g_stack.back().items;
			if (g_cursor < items.size() && items[g_cursor].hover)
				items[g_cursor].hover();
		}

		void Move(int by)
		{
			const auto& items = g_stack.back().items;
			if (items.empty())
				return;
			const size_t n = items.size();
			g_cursor = (g_cursor + n + static_cast<size_t>(by + static_cast<int>(n))) % n;
			AUDIO::PLAY_SOUND_FRONTEND(-1, "NAV_UP_DOWN", "HUD_FRONTEND_DEFAULT_SOUNDSET", true);
			Hover();
		}

		// The controls the menu takes over while it is open: the phone, weapon selection and firing.
		void Block()
		{
			for (const int c : {kUp, kDown, kSelect, kBack, 174, 175, 27 /* phone */, 14, 15, 16, 17 /* weapon wheel */, 99, 100,
			         kWheelUp, kWheelDown, kAttack, kAim, 140, 141, 142 /* melee */, 199, 200 /* pause */, 37 /* weapon select */})
				PAD::DISABLE_CONTROL_ACTION(0, c, true);
		}
	}

	void Open(List list)
	{
		if (!g_stack.empty())
			g_cursors.push_back(g_cursor);
		g_stack.push_back(std::move(list));
		g_cursor = 0;
		g_openedAt = ml::TickMs();
		AUDIO::PLAY_SOUND_FRONTEND(-1, "SELECT", "HUD_FRONTEND_DEFAULT_SOUNDSET", true);
		Hover();
	}

	void Close()
	{
		auto stack = std::move(g_stack); // a leave callback may open another menu
		g_stack.clear();
		g_cursors.clear();
		g_cursor = 0;
		for (auto it = stack.rbegin(); it != stack.rend(); ++it)
			if (it->leave)
				it->leave();
	}

	bool IsOpen()
	{
		return !g_stack.empty();
	}

	const List* Current(size_t& cursor)
	{
		cursor = g_cursor;
		return g_stack.empty() ? nullptr : &g_stack.back();
	}

	void Tick()
	{
		static uint64_t lastOpen = 0;
		if (g_stack.empty())
		{
			// The key that closed the menu is still down for a few frames: kept from the game (Backspace or Enter
			// in a vehicle got the officer out or in).
			if (ml::TickMs() - lastOpen < 350)
				Block();
			return;
		}
		lastOpen = ml::TickMs();
		Block();
		const List& list = g_stack.back();
		const auto& items = list.items;
		if (!items.empty())
			g_cursor = std::min(g_cursor, items.size() - 1);

		// Drawing.
		Rect(kLeft, kTop, kWidth, kHeader, 0, 0, 0, 220);
		Rect(kLeft, kTop + kHeader - 0.004f, kWidth, 0.004f, 93, 182, 229, 255);
		Text(list.title, kLeft + 0.008f, kTop + 0.008f, 0.5f, 255, 255, 255);
		const size_t first = g_cursor >= kVisible ? g_cursor - kVisible + 1 : 0;
		float y = kTop + kHeader;
		for (size_t i = first; i < items.size() && i < first + kVisible; ++i, y += kRow)
		{
			const Item& it = items[i];
			const bool sel = i == g_cursor;
			Rect(kLeft, y, kWidth, kRow, sel ? 240 : 0, sel ? 240 : 0, sel ? 240 : 0, sel ? 235 : 170);
			const int c = it.enabled ? (sel ? 0 : 255) : 130;
			Text(it.label + (it.sub ? "  >" : ""), kLeft + 0.008f, y + 0.004f, 0.36f, c, c, c);
			if (!it.detail.empty())
				Text(it.detail, kLeft + kWidth - 0.008f, y + 0.004f, 0.36f, c, c, c, true);
		}
		if (items.empty())
		{
			Rect(kLeft, y, kWidth, kRow, 0, 0, 0, 170);
			Text("（沒有可用的項目）", kLeft + 0.008f, y + 0.004f, 0.36f, 130, 130, 130);
			y += kRow;
		}
		if (items.size() > kVisible)
		{
			Rect(kLeft, y, kWidth, 0.022f, 0, 0, 0, 200);
			Text(std::to_string(g_cursor + 1) + " / " + std::to_string(items.size()), kLeft + kWidth / 2 - 0.012f, y + 0.002f, 0.3f, 200, 200, 200);
			y += 0.022f;
		}
		if (!items.empty() && !items[g_cursor].help.empty())
		{
			Rect(kLeft, y + 0.004f, kWidth, kRow, 0, 0, 0, 200);
			Text(items[g_cursor].help, kLeft + 0.008f, y + 0.008f, 0.3f, 200, 200, 200);
		}

		// Input (not in the frame the menu opened, so the opening key does not choose too).
		if (ml::TickMs() - g_openedAt < 150)
			return;
		if (Pressed(kUp) || Pressed(kWheelUp))
			Move(-1);
		else if (Pressed(kDown) || Pressed(kWheelDown))
			Move(1);
		else if ((Pressed(174) || Pressed(175)) && !items.empty() && items[g_cursor].step)
		{
			AUDIO::PLAY_SOUND_FRONTEND(-1, "NAV_LEFT_RIGHT", "HUD_FRONTEND_DEFAULT_SOUNDSET", true);
			const auto step = items[g_cursor].step;
			step(Pressed(174) ? -1 : 1);
		}
		else if (Pressed(kBack) || Pressed(kAim))
		{
			AUDIO::PLAY_SOUND_FRONTEND(-1, "BACK", "HUD_FRONTEND_DEFAULT_SOUNDSET", true);
			const auto leave = g_stack.back().leave;
			g_stack.pop_back();
			if (!g_cursors.empty())
			{
				g_cursor = g_cursors.back();
				g_cursors.pop_back();
			}
			if (leave)
				leave();
		}
		else if ((Pressed(kSelect) || Pressed(kAttack)) && !items.empty())
		{
			const Item it = items[g_cursor]; // the action may change the menus
			if (!it.enabled)
			{
				AUDIO::PLAY_SOUND_FRONTEND(-1, "ERROR", "HUD_FRONTEND_DEFAULT_SOUNDSET", true);
				return;
			}
			if (it.sub)
			{
				Open(it.sub());
				return;
			}
			AUDIO::PLAY_SOUND_FRONTEND(-1, "SELECT", "HUD_FRONTEND_DEFAULT_SOUNDSET", true);
			if (!it.keepOpen)
				Close();
			if (it.action)
				it.action();
		}
	}
}
