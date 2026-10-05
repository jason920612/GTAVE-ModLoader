#pragma once
#include <Windows.h>

namespace loader::ui
{
	// Installs the DXGI hooks. Call once, from any thread, after the game window exists.
	bool StartOverlay();

	// True while the loader UI owns input (landing replacement or the F4 window).
	bool CapturesInput();

	// The window the game presents into (nullptr until the overlay attached).
	HWND GameWindow();
}
