#pragma once
// The graphics mod's bridge between the game and the UE5 renderer (research/phase0.md §59.5): plain C++, shared by
// the loader (src/loader/graphics/bridge.cpp) and the renderer (GTAVBridgeSubsystem).
//
// The loader creates the shared memory "Local\GTAVUE5Bridge-<game process id>" and starts the renderer with
// -gtavbridge=<game process id>. Each game frame the loader writes the camera and the world's state and bumps
// gameSequence. The renderer draws that view at width x height, copies the HDR scene colour (linear, before tone
// mapping: the game does its own post-processing and its UI on top) into a shared texture, signals a shared fence
// with frameValue and publishes it. In the game's DLSS pass the loader makes the game's queue wait for that value
// and copies the texture over DLSS's output.
#include <cstdint>

namespace GTAVBridge
{
	constexpr uint32_t kMagic = 0x31425547; // "GUB1"
	constexpr uint32_t kVersion = 3;
	constexpr uint32_t kMaxGroups = 1024;

	// Names: shared memory, and the renderer's texture and fence (NT handles by name; <pid> is the game's process id).
	inline constexpr wchar_t kMemoryName[] = L"Local\\GTAVUE5Bridge-";
	inline constexpr wchar_t kTextureName[] = L"Local\\GTAVUE5Frame-";
	inline constexpr wchar_t kFenceName[] = L"Local\\GTAVUE5Fence-";

	struct Shared
	{
		uint32_t magic;
		uint32_t version;

		// --- the game (loader) writes ---------------------------------------------------------------------
		volatile uint32_t gameSequence; // bumped after each frame's view is written
		uint32_t enabled;               // 0: the renderer may idle
		uint32_t width, height;         // the size the game composes (DLSS's output)
		float position[3];              // camera, the game's space (metres)
		float rotation[3];              // (unused: forward and up below)
		float fov;                      // vertical, degrees
		float nearClip, farClip;
		float hours;                    // 0..24 (minutes as a fraction)
		uint32_t weatherFrom, weatherTo; // weather type name hashes
		float weatherBlend;              // 0: from, 1: to
		uint32_t groupCount;
		uint32_t groupsOn[kMaxGroups];   // name hashes of the map data groups on
		volatile uint32_t groupsSequence; // bumped when groupsOn changed

		// --- the renderer writes -----------------------------------------------------------------------
		volatile uint32_t rendererState; // 0 starting, 1 rendering, 2 failed
		volatile uint32_t textureSequence; // bumped when the shared texture was (re)made (size change)
		uint32_t textureWidth, textureHeight;
		volatile uint64_t frameValue;      // the fence value of the newest frame in the texture
		volatile uint32_t frameSequence;   // the gameSequence that frame was drawn for

		// --- the game (loader) writes: the camera the game rendered with (Streamline's constants) -------------
		float forward[3], up[3]; // unit vectors, the game's space
		float aspect;
		// Console commands for the renderer (the loader's ModLoader/ue5console.txt, one per line), bumped when new.
		volatile uint32_t consoleSequence;
		char console[1024];
		uint32_t reserved[8];
	};
}
