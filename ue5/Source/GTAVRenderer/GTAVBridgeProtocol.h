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
	constexpr uint32_t kVersion = 13;
	constexpr uint32_t kMaxGroups = 1024;
	constexpr uint32_t kMaxProxies = 64;
	constexpr uint32_t kTextures = 3; // the renderer's frame for game frame s is in texture s % kTextures
	constexpr uint32_t kViews = 8;
	constexpr uint32_t kMaxGameLights = 1024; // (as many as the game's own list holds)
	constexpr uint32_t kLightLists = 4;
	constexpr uint32_t kMaxInteriors = 24;      // interiors whose entity sets are passed (the nearest)
	constexpr uint32_t kMaxInteriorSets = 48;   // entity sets on, per interior
	constexpr uint32_t kMaxObjects = 512;       // the game's objects passed (the nearest)
	constexpr uint32_t kObjectLists = 4;

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
		uint32_t lights;                 // the game's newest light list then (lightLists[lights % kLightLists]; 0: none)
		int64_t time;                    // when the game set it (QueryPerformanceCounter: the same clock in both processes)
	};

	// A light the game drew in a frame (its scene lights: what is on and how bright, after its own switching by the
	// hour, by scripts, its fades and flicker; a vehicle's lamps among them).
	struct GameLight
	{
		float position[3];          // the game's space (metres)
		uint32_t type;              // the game's: 1 point, 2 spot, 4 capsule
		float direction[3];         // unit: where a spot shines
		float intensity;            // the game's, as it drew it
		float tangent[3];           // unit: a capsule's axis
		float radius;               // metres: its reach
		float color[3];             // 0..1
		float falloffExponent;      // the game's
		float coneInner, coneOuter; // degrees from the axis (spots)
		float length;               // metres (capsules)
		uint32_t flags;             // the game's
	};
	static_assert(sizeof(GameLight) == 80);

	// An interior of the game's near its camera, and the entity sets the game has on in it (scripts switch them: the
	// rooms of Michael's house as the story goes - none of its 17 is on by default - a shop's stock, an office's look).
	struct InteriorSets
	{
		float position[3]; // where the interior is placed (the game's space, metres)
		uint32_t name;     // its MLO archetype's name hash
		uint32_t count;
		uint32_t sets[kMaxInteriorSets]; // name hashes of its entity sets on
	};

	// An object of the game's (its CObject pool) near its camera that is not as the map placed it: moved - a door swung,
	// a bin knocked over, a buoy on the waves - hidden, or not the map's at all (a script's, a mission's).
	struct GameObject
	{
		uint32_t id;                         // the pool slot and its generation: the same object from frame to frame
		uint32_t model;                      // its archetype's name hash
		float position[3];                   // the game's space (metres)
		float right[3], forward[3], up[3];   // its axes (their lengths its scale)
		float placed[3];                     // where the map placed it (the dummy it was made from); NaN: not the map's
		uint32_t flags;                      // kObjectVisible
	};
	static_assert(sizeof(GameObject) == 72);
	constexpr uint32_t kObjectVisible = 1;

	// One frame's objects (sequence 0 while it is being written).
	struct ObjectList
	{
		volatile uint32_t sequence;
		uint32_t count;
		GameObject objects[kMaxObjects];
	};

	// One frame's lights (sequence 0 while it is being written).
	struct LightList
	{
		volatile uint32_t sequence;
		uint32_t count;
		GameLight lights[kMaxGameLights];
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
		// The game's lights, a list a frame: the newest complete one is lightLists[lightSequence % kLightLists] (0: the
		// loader could not get them; the renderer then lights by the map's data alone).
		volatile uint32_t lightSequence;
		LightList lightLists[kLightLists];
		// The game's wet ground: 0 dry - 1 soaked (rises in the rain, dries over about an hour of game time after it);
		// negative when the loader could not find it.
		float wetness;
		// The game's interiors near its camera with their entity sets on, a few times a second (sequence bumped after
		// each update; 0: the loader cannot read them, the renderer then shows an interior's default sets).
		volatile uint32_t interiorSequence;
		uint32_t interiorCount;
		InteriorSets interiors[kMaxInteriors];
		// The game's objects near its camera not as the map placed them, a list a frame: the newest complete one is
		// objectLists[objectSequence % kObjectLists] (0: the loader cannot read them; the renderer draws the map as placed).
		volatile uint32_t objectSequence;
		ObjectList objectLists[kObjectLists];
	};
}
