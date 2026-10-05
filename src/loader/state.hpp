#pragma once
#include <atomic>
#include <cstdint>

// State shared between the game thread (writer) and the UI/render thread (reader).
namespace loader::state
{
	inline std::atomic_bool landing = false;        // the game's landing page is showing
	inline std::atomic_bool story = false;          // story mode is running (main_persistent exists)
	inline std::atomic_bool online = false;         // GTA Online detected: mods paused
	inline std::atomic_bool storyLoading = false;   // continue-story was issued; waiting for story mode
	inline std::atomic_bool menuOpen = false;       // loader UI currently owns input
	inline std::atomic<uint32_t> nativesResolved = 0;
	inline std::atomic<uint32_t> crossmapEntries = 0;
}
