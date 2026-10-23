#pragma once
#include <set>
#include <string>

#include "log.hpp"

namespace loader::config
{
	struct Config
	{
		log::Level logLevel = log::Level::Info;
		std::string menuKey = "F4";
		std::set<std::string> disabledMods;   // file names in ModLoader\mods
		std::set<std::string> disabledAssets; // DLC pack folder names in ModLoader\mods
		// Public -> Enhanced native hash table, refreshed on every launch (kept if the download fails).
		std::string crossmapUrl = "https://raw.githubusercontent.com/YimMenu/YimMenuV2/enhanced/src/game/gta/invoker/crossmap.txt";
		bool crossmapAutoUpdate = true;
		// Show the loader's home screen instead of the game's (online-focused) landing page.
		bool replaceLandingPage = true;
		// Diagnostics only: skip hooking the game's script loop (mods will not run).
		bool debugDisableScriptHook = false;
		// Research only: log writes to the landing page flow state (hardware breakpoints).
		bool debugWatchLanding = false;
		// Research only: arm watches listed in ModLoader\debug_watch.txt.
		bool debugWatchFile = false;
		// Research only: from startup, log writes to the pause menu screen array globals.
		bool debugWatchBoot = false;
		// Research only: log writes to a posed bone's matrices after the pose override is applied (research §45).
		bool debugPoseWatch = false;
		// Research only: log the NVIDIA NGX core's file calls, Streamline / DLSS DLL loads and DLSS calls (research §51).
		bool debugDlssTrace = false;
		// Testing only: a tester controls the game through a named pipe (keys, controls, screenshots, the window off
		// screen) while the person keeps using the computer (debug/remote.hpp).
		bool debugRemote = false;
		// Story-mode pause menu: the "Online" tab becomes a "Mods" tab with each mod's settings.
		bool pauseMenuModsTab = true;
		// The phone's / computers' web browser is replaced by the loader's (ModLoader\browser, pages in ModLoader\web).
		bool replaceBrowser = true;
		// The local text model mods can use (ModLoader\ai, downloaded from the settings page).
		bool aiEnabled = false;
		// DLSS: use the newest DLSS the NVIDIA driver / app keeps in C:\ProgramData\NVIDIA\NGX\models instead of the
		// game's copies (research §51). Takes effect at the next start.
		bool dlssUpgrade = true;
		// DLSS Super Resolution model: "auto" (K for DLAA / Quality / Balanced, M for Performance, L for Ultra
		// Performance; needs DLSS 310.5 or newer), "game" (what the game asks for) or a preset letter ("K", "L", "M").
		std::string dlssPreset = "auto";
		// The fourth story character (game/char4.hpp): the loader's pack carries the game scripts rewritten for it.
		// Without it a mod's own character (the police mod's officer) has no money, shops or activities of its own.
		bool fourthCharacter = true;
		// Graphics mod (research §58, §59): the game's 3D scene drawn by Unreal Engine 5 from the pack files
		// tools/ue5export made (graphics/bridge.hpp). Needs the game's DLSS on. Takes effect at the next start.
		bool ue5Renderer = false;
		std::string ue5Editor;  // UnrealEditor.exe
		std::string ue5Project; // GTAVRenderer.uproject (tools/ue5_setup.py)
		std::string ue5Pack;    // the pack folder (ue5export pack)
		std::string ue5Key = "F11"; // switches between UE5's picture and the game's (F6-F8: the trainer's, F9-F10: the police mod's)
		// The renderer's quality: "low", "medium", "high", "ultra", "cinematic" (graphics/bridge.cpp: resolution, the
		// engine's scalability groups, draw distance, lights). Applied while it runs.
		std::string ue5Preset = "high";
		// How far the renderer's picture is brought to the brightness of the game's own scene (0: the renderer's own
		// exposure, 1: the game's).
		float ue5ExposureMatch = 1.0f;
		// While UE5 draws the world the game's own 3D picture only carries its peds and vehicles: the DLSS mode the
		// game renders for then ("game": its own setting; "quality", "balanced", "performance"), which sets the game's
		// internal resolution. Takes effect when the game starts. (Measured at Legion Square, high preset: the game's
		// Quality 60 frames/s, Performance 53: lower is not faster there, hence "game".)
		std::string ue5GameResolution = "game";
		// Its world's detail meanwhile: "lean" (what only draws the world off: grass, water, tessellation, volumetric
		// fog, the farther levels of detail; the peds' and vehicles' own kept) or "game" (its own settings). Takes
		// effect when the game starts.
		std::string ue5GameDetail = "lean";
	};

	// Loads loader.json, writing a default one if it is missing or unreadable.
	Config& Load();
	Config& Get();
	bool Save();
}
