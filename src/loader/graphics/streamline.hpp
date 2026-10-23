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

	// Called on the render thread right after the game's DLSS Super Resolution pass was recorded into commandList:
	// output is DLSS's output colour (the HDR scene before the game's post-processing and UI), in outputState.
	using AfterDlss = void (*)(void* commandList, void* output, uint32_t outputState);
	void SetAfterDlss(AfterDlss callback);
}
