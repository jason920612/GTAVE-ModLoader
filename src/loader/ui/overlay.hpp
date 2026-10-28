#pragma once
#include <Windows.h>

#include <string>

struct ID3D12Device;

namespace loader::ui
{
	// The DXGI hooks put in early - at the loader's start, before the overlays that attach to every new swap chain are
	// loaded (Social Club's renderer, at ~18 s, ran into the probe swap chain made at the first script tick and stopped the
	// game, research/phase0.md §66.4) -; the game's frames go through untouched till StartOverlay. Any thread but the loader
	// lock's.
	void PrepareOverlay();

	// The UI started (the DXGI hooks put in now if PrepareOverlay did not). Call from any thread once the game window exists.
	bool StartOverlay();

	// True while the loader UI owns input (landing replacement or the F4 window).
	bool CapturesInput();

	// The window the game presents into (nullptr until the overlay attached).
	HWND GameWindow();

	// The game's D3D12 device (nullptr until the overlay attached).
	ID3D12Device* Device();

	// Saves the next frame the game presents as a PNG (waits up to 5 s for it). Any thread but the render thread.
	bool Capture(const std::wstring& path, std::string& error);
}
