#pragma once
#include <Windows.h>

#include <cstdint>

// The game's per-frame Streamline calls (sl.interposer.dll), which the UE5 renderer bridge builds on (research/phase0.md
// §59.5): slSetConstants carries the frame's camera, slSetTag / slSetTagForFrame the buffers DLSS works on (and the
// command list they are recorded on), slEvaluateFeature the DLSS passes themselves.
namespace loader::graphics::streamline
{
	// Hooks those exports (once); trace: log one frame's calls every few seconds.
	void Hook(HMODULE interposer, bool trace);

	// The last camera the game gave Streamline (the game's world space, metres).
	struct Camera
	{
		uint32_t frame = 0;
		float position[3]{}, up[3]{}, right[3]{}, forward[3]{}; // forward: as the game gives it (the camera's back)
		float nearZ = 0, farZ = 0, fov = 0, aspect = 0; // fov: vertical, radians
		float viewToClip[16]{};
		bool valid = false;
	};
	Camera LastCamera();
	// Called on the render thread with each camera the game gives Streamline (before that frame's DLSS pass).
	using OnCamera = void (*)(const Camera& camera);
	void SetOnCamera(OnCamera callback);

	// One DLSS pass: the command list it was recorded into and the buffers the game tagged for it (ID3D12Resource*,
	// with the state the game says each is in).
	struct DlssFrame
	{
		void* commandList = nullptr;
		void* output = nullptr; // DLSS's output colour: the HDR scene before the game's post-processing and UI
		uint32_t outputState = 0;
		void* input = nullptr; // the scene at the render resolution
		uint32_t inputState = 0;
		void* depth = nullptr; // depth and stencil at the render resolution
		uint32_t depthState = 0;
		void* bias = nullptr; // where DLSS is to favour this frame's colour over its history (R8, the render resolution)
		uint32_t biasState = 0;
		uint32_t renderWidth = 0, renderHeight = 0; // the part of the inputs in use
	};
	// Called on the render thread right after the game's DLSS Super Resolution pass was recorded.
	using AfterDlss = void (*)(const DlssFrame& frame);
	void SetAfterDlss(AfterDlss callback);
}
