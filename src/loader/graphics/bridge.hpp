#pragma once
#include <cstdint>

// The graphics mod's bridge to the UE5 renderer (research/phase0.md §59.5): with loader.json "ue5Renderer" on, the
// loader starts the renderer (ue5/, the pack files ue5export made), gives it the game's view and world state every
// frame, and puts its picture in place of the game's 3D scene right after the game's DLSS pass, so that the game's
// own post-processing and its UI come on top. The hotkey ("ue5Key") switches between the two pictures.
namespace loader::graphics::bridge
{
	// Once at start: hooks the DLSS pass (needs the game's DLSS on).
	void Start();
	// Game thread, story mode, every frame: starts the renderer when it is needed, writes the view and the state.
	void Tick();
	// A key the person pressed (or a tester sent).
	void OnKeyDown(uint32_t vk);
	// The game closes: the renderer goes too.
	void Stop();
	// The renderer's picture is the one shown.
	bool Showing();
}
