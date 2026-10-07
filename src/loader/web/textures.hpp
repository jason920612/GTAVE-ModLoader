#pragma once
#include <cstdint>
#include <functional>
#include <string>

#include <d3d12.h>

// Game textures as images for web pages (https://gametextures/<dictionary>/<texture>.png). A texture is drawn as a
// sprite over a black and then a white square while the browser covers the screen, read back from the frame, and
// saved (with the alpha worked out from the two) to ModLoader\browser\cache\textures\<dictionary>\<texture>.png.
namespace loader::web::textures
{
	// Asks for a texture; `done` gets the PNG file ("" when there is no such texture). `done` runs on the game thread,
	// or right away when the file is cached. Any thread.
	void Request(const std::string& dictionary, const std::string& texture, std::function<void(const std::wstring& file)> done);

	// Game thread, inside a script context, every frame.
	void Tick();

	// Render thread, before the overlay draws: may record a copy of the back buffer (in PRESENT state).
	// `signal` is the fence value this frame's work will signal, `completed` the value the GPU reached.
	void OnRender(ID3D12Device* device, ID3D12GraphicsCommandList* list, ID3D12Resource* backBuffer, uint64_t signal, uint64_t completed);
}
