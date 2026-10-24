#pragma once
#include <d3d12.h>

#include <string>

// The game's particles (fire, smoke, explosions, sparks, muzzle flashes, rain) for the UE5 renderer's picture (research/
// phase0.md §60.11, §60.21): the renderer draws none of them, and the composite kept only the game's peds and vehicles, so
// a car burned unseen. The game draws its particles into its scene colour (the colour DLSS is given) late in the frame,
// in command lists recorded in parallel, among its other see-through things (windows, light shafts, lamps' coronas). Each
// particle it draws there is drawn again, with the same pipeline and bindings, into a layer of our own - two targets, one
// cleared to black and one to white: what the particles add, and how much of what is behind them shows through - read
// after DLSS and cleared for the next frame. (Copying the scene colour before and after the lists that drew particles took
// in what else those lists drew, in some frames and not others: research §60.21.)
// The pipelines they are drawn with are known by their pixel shaders (the digests in particle_shaders.inc, made from the
// game's shader container by tools/ptfx_shaders.py). D3D12's own functions are hooked (a probe device's: the class's,
// the same for the game's objects), so the game's calls are seen whatever wraps its objects.
// Its decals (bullet holes, blood, scorch marks) by copies (research §60.17): the game draws them onto its world's
// surfaces in a list of their own, into its G-buffer (decal_shaders.inc, tools/decal_shaders.py); the G-buffer's first
// target - the surfaces' colour - is copied before and after it (small lists of our own put in among the game's as it
// submits them: copies inside the game's own lists hung the GPU, research §60.15), and how much they darkened or tinted
// each pixel goes onto the renderer's same surface.
namespace loader::graphics::particles
{
	// Hooks D3D12 once d3d12.dll is loaded (from the start: the game makes its pipelines as it loads).
	void Start();
	// The scene colour the game gives DLSS (its particles are drawn into it); set each frame before the next.
	void SetScene(ID3D12Resource* scene);
	// This frame's particles' layer and decals' copies, after DLSS, with the command list DLSS was recorded on (its
	// submission ends the frame): readable (NON_PIXEL_SHADER_RESOURCE) in that list until FinishLayer.
	struct Layer
	{
		// the particles drawn over black (what they add) and over white (that, plus how much of what is behind shows
		// through), the scene colour's size; null when there is none to read
		ID3D12Resource* particles = nullptr;
		ID3D12Resource* particlesWhite = nullptr;
		D3D12_CPU_DESCRIPTOR_HANDLE particleViews[2]{};
		// the G-buffer's surface colour before and after the decals (the same; null when there are none)
		ID3D12Resource* decalBefore = nullptr;
		ID3D12Resource* decalAfter = nullptr;
		bool decalSrgb = false; // (the decals' copies hold sRGB-encoded colours)
	};
	Layer TakeLayer(ID3D12GraphicsCommandList* list);
	// The particles' layer back to a render target and cleared, in the same list after what read it (every frame, read
	// or not).
	void FinishLayer(ID3D12GraphicsCommandList* list, const Layer& layer);
	// The particles (decals) over the renderer's picture or not (research: the remote's "particles 0|1", "decals 0|1").
	void SetParticles(bool on);
	void SetDecals(bool on);
	// Research: the next two frames' pipelines and render targets, list by list, to a file.
	void Trace(const std::wstring& path);
}
