#pragma once
#include <d3d12.h>

#include "streamline.hpp"

// The UE5 renderer's picture put over DLSS's output, keeping what the renderer does not draw: the game's peds and
// vehicles, found by the stencil the game gave DLSS (research/phase0.md §59.6), and brought to the brightness of the
// game's own scene. Compute passes on the game's command list, right after DLSS (which leaves the list's state for the
// game to set again anyway).
namespace loader::graphics::composite
{
	// Records the pass; picture: the renderer's frame (DLSS's output size and format); match: how far its brightness
	// is brought to the game's (0..1). False when it cannot (an unknown depth format, no device objects): the caller
	// copies the picture instead.
	bool Record(const streamline::DlssFrame& frame, ID3D12Resource* picture, float match);
}
