#pragma once
#include <d3d12.h>
#include <dxgi1_4.h>

namespace loader::ui::dx12
{
	struct Callbacks
	{
		// Called on the render thread right before the game's frame is presented.
		void (*onPresent)(IDXGISwapChain3* swapChain, ID3D12CommandQueue* queue);
		// Called before the swap chain's buffers are resized or the swap chain goes away:
		// every reference to its back buffers must be released here.
		void (*onReleaseBuffers)(IDXGISwapChain3* swapChain);
	};

	// Discovers the DXGI swap chain vtable and where a swap chain keeps its command queue
	// (using a throwaway device), then patches Present/ResizeBuffers in that vtable.
	// Only data in dxgi.dll is modified; game code is never patched.
	bool Install(const Callbacks& callbacks);
}
