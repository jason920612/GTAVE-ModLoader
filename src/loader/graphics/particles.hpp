#pragma once
#include <d3d12.h>

#include <string>

// The game's particles (fire, smoke, explosions, sparks, muzzle flashes, rain) for the UE5 renderer's picture (research/
// phase0.md §60.11): the renderer draws none of them, and the composite kept only the game's peds and vehicles, so a car
// burned unseen. The game draws its particles into its scene colour (the colour DLSS is given) late in the frame, in
// command lists recorded in parallel (two to six of them, as many as it has particles for), among its other see-through
// things (vehicle glass after them, say). The scene colour is copied before the first list with particles the GPU runs
// and after the last: their difference is what the particles did (with what else those lists drew). The copies are
// small lists of our own put in among the game's as it submits them; inside the game's own lists (around each run of
// particles) they hung the GPU twice in night soaks (research §60.15).
// The pipelines they are drawn with are known by their pixel shaders (the digests in particle_shaders.inc, made from the
// game's shader container by tools/ptfx_shaders.py). D3D12's own functions are hooked (a probe device's: the class's,
// the same for the game's objects), so the game's calls are seen whatever wraps its objects.
namespace loader::graphics::particles
{
	// Hooks D3D12 once d3d12.dll is loaded (from the start: the game makes its pipelines as it loads).
	void Start();
	// The scene colour the game gives DLSS (its particles are drawn into it); set each frame before the next.
	void SetScene(ID3D12Resource* scene);
	// This frame's copies before and after the particles (in NON_PIXEL_SHADER_RESOURCE state, the scene's size and
	// format; null when it drew none). Called once a frame, after DLSS, with the command list DLSS was recorded on (its
	// submission ends the frame's particles).
	struct Layer
	{
		ID3D12Resource* before = nullptr;
		ID3D12Resource* after = nullptr;
	};
	Layer TakeLayer(ID3D12GraphicsCommandList* list);
	// Research: the next two frames' pipelines and render targets, list by list, to a file.
	void Trace(const std::wstring& path);
}
