#pragma once
#include <cstdint>
#include <string>

struct ID3D12CommandQueue;
struct IDXGISwapChain3;

// Development aid (debug/remote "record"): the frames the game presents, as a video file for tools/gtactl.py to turn into
// an MP4 - what only shows while things move (stutter, things sliding against each other, flicker). The copies are made
// on the GPU into a ring of readback buffers and written by a worker thread, so the game does not wait for them; a frame
// that finds the ring full is left out (and counted). Every present's time is kept, recorded or not.
namespace loader::ui::recorder
{
	// Starts recording `seconds` of frames, at most `fps` of them a second, each scaled down by `scale` (1-4), into `path`.
	bool Start(const std::wstring& path, double seconds, double fps, int scale, std::string& error);
	// True while a recording is under way (frames still to take or to write).
	bool Busy();
	// The render thread, right before the game's present (after the overlay drew).
	void OnPresent(IDXGISwapChain3* swapChain, ID3D12CommandQueue* queue);
	// What a frame of the recording is tagged with (the UE5 bridge's view of that frame), set on the render thread.
	void SetFrameInfo(uint32_t a, uint32_t b, uint32_t c, uint32_t d);
}
