#pragma once
#include <Windows.h>

struct ID3D12Device;

namespace loader::ui
{
	// Installs the DXGI hooks (only the first call does anything). Call from any thread once the game window exists.
	bool StartOverlay();

	// True while the loader UI owns input (landing replacement or the F4 window).
	bool CapturesInput();

	// The window the game presents into (nullptr until the overlay attached).
	HWND GameWindow();

	// The game's D3D12 device (nullptr until the overlay attached).
	ID3D12Device* Device();
}
