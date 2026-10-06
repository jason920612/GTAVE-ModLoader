#pragma once
#include <string>

namespace loader::ui
{
	// Queues a short on-screen message (shown for a few seconds above the minimap). Any thread.
	void Notify(std::string source, std::string text);

	// Window thread, on WM_KEYDOWN: while the player is rebinding a mod hotkey, takes the key and
	// returns true (Esc cancels, Backspace unbinds).
	bool CaptureHotkey(unsigned vk);
}
