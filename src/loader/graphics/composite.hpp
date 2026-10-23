#pragma once
#include <d3d12.h>

#include "streamline.hpp"

// The UE5 renderer's picture put over DLSS's output, keeping what the renderer does not draw: the game's peds and
// vehicles, found by the stencil the game gave DLSS (research/phase0.md §59.6), where the renderer's world is not in
// front of them (its depth in the picture's alpha), and brought to the brightness, the colour balance and the highlights
// of the game's own scene. Compute passes on the game's command list, right after DLSS (which leaves the list's state for the
// game to set again anyway).
namespace loader::graphics::composite
{
	struct Settings
	{
		float match = 1, grade = 1; // how far the brightness and the colour balance are brought to the game's (0..1)
		// The game's depth d (its projection, reversed) to metres along the view: numerator / (d scale - offset).
		float depthNumerator = 0.1f, depthScale = -1, depthOffset = 0;
	};
	// Records the pass; picture: the renderer's frame (DLSS's output size and format, its depth in metres in alpha).
	// False when it cannot (an unknown depth format, no device objects): the caller copies the picture instead.
	bool Record(const streamline::DlssFrame& frame, ID3D12Resource* picture, const Settings& settings);
}
