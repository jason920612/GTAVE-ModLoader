#pragma once
// The graphics mod's bridge between the game and the UE5 renderer (research/phase0.md §59.5): plain C++, shared by
// the loader (src/loader/graphics/bridge.cpp) and the renderer (GTAVBridgeSubsystem).
//
// The loader creates the shared memory "Local\GTAVUE5Bridge-<game process id>" and starts the renderer with
// -gtavbridge=<game process id>. Each game frame the loader writes that frame's view into views[sequence % kViews] and
// bumps gameSequence. The renderer draws every view in turn (the newest when it fell far behind) at width x height,
// puts the HDR scene colour (linear, before tone mapping: the game does its own post-processing and its UI on top) and
// its depth into the shared texture sequence % kTextures, signals a shared fence with the sequence and publishes it.
// In the game's DLSS pass the loader makes the game's queue wait for its own frame's value and composes that texture
// with DLSS's output; when the frame has been presented, the game's queue signals the second fence ("consumed") with
// it, and the renderer does not draw into a texture the game may still read.
#include <cstdint>

namespace GTAVBridge
{
	constexpr uint32_t kMagic = 0x31425547; // "GUB1"
	constexpr uint32_t kVersion = 8;
	constexpr uint32_t kMaxGroups = 1024;
	constexpr uint32_t kMaxProxies = 64;
	constexpr uint32_t kTextures = 3; // the renderer's frame for game frame s is in texture s % kTextures
	constexpr uint32_t kViews = 8;

	// A ped or vehicle near the player: the game's picture shows it, the renderer casts its shadow on its own ground
	// (and a vehicle's lamps light it).
	struct Proxy
	{
		float position[3];                // the game's space (metres)
		float forward[3], up[3];          // unit
		float boundsMin[3], boundsMax[3]; // the model's (local: x right, y forward, z up)
		uint32_t kind;                    // 1 ped, 2 vehicle
		uint32_t lights;                  // a vehicle's: 1 headlights on, 2 full beam (GET_VEHICLE_LIGHTS_STATE)
	};

	// The camera a game frame was rendered with (Streamline's constants).
	struct View
	{
		uint32_t sequence;               // the game frame's
		float position[3];               // the game's space (metres)
		float forward[3], up[3];         // unit vectors, the game's space
		float fov;                       // vertical, degrees
		float aspect;
		float nearClip, farClip;
	};

	// Names: shared memory, and the renderer's textures and fences (NT handles by name; <pid> is the game's process id).
	inline constexpr wchar_t kMemoryName[] = L"Local\\GTAVUE5Bridge-";
	// (kTextureName + pid + "-0" .. "-2")
	inline constexpr wchar_t kTextureName[] = L"Local\\GTAVUE5Frame-";
	inline constexpr wchar_t kFenceName[] = L"Local\\GTAVUE5Fence-";
	inline constexpr wchar_t kConsumedName[] = L"Local\\GTAVUE5Consumed-";

	struct Shared
	{
		uint32_t magic;
		uint32_t version;

		// --- the game (loader) writes ---------------------------------------------------------------------
		volatile uint32_t gameSequence; // bumped after each frame's view is written
		uint32_t enabled;               // 0: the renderer may idle
		uint32_t width, height;         // the size the game composes (DLSS's output)
		float hours;                    // 0..24 (minutes as a fraction)
		uint32_t weatherFrom, weatherTo; // weather type name hashes
		float weatherBlend;              // 0: from, 1: to
		uint32_t groupCount;
		uint32_t groupsOn[kMaxGroups];   // name hashes of the map data groups on
		volatile uint32_t groupsSequence; // bumped when groupsOn changed
		View views[kViews];              // views[s % kViews]: game frame s's

		// --- the renderer writes -----------------------------------------------------------------------
		volatile uint32_t rendererState; // 0 starting, 1 rendering, 2 failed
		volatile uint32_t textureSequence; // bumped when the shared textures were (re)made (size change)
		uint32_t textureWidth, textureHeight;
		volatile uint64_t frameValue;      // the fence value (the game frame) of the newest frame drawn
		volatile uint32_t frameSequences[kTextures]; // the game frame each texture holds

		// --- the game (loader) writes ---------------------------------------------------------------------
		// Console commands for the renderer (the loader's ModLoader/ue5console.txt, one per line), bumped when new.
		volatile uint32_t consoleSequence;
		char console[1024];
		// The game's wind (GET_WIND_DIRECTION, GET_WIND_SPEED): the way it blows (the game's space), m/s.
		float wind[3];
		float windSpeed;
		// The game's date (GET_CLOCK_YEAR, _MONTH + 1, _DAY_OF_MONTH): the moon's phase.
		uint32_t year, month, day;
		// The interior the game's camera is in (GET_INTERIOR_FROM_PRIMARY_VIEW, 0 outside) and its room
		// (GET_ROOM_KEY_FOR_GAME_VIEWPORT): no rain, no wet floors, no outdoor haze in there.
		int32_t interior;
		uint32_t room;
		// A lightning flash: how much brighter than of late the game's own sky (or, with none in view, its picture) is
		// this moment, in stops (0: none; a few frames late: measured on the game's GPU, read back).
		float lightning;
		// The peds and vehicles near the player (their shadow casters), each frame.
		uint32_t proxyCount;
		Proxy proxies[kMaxProxies];
	};
}
