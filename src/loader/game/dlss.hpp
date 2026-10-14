#pragma once
#include <cstdint>
#include <string>

// Newer DLSS (research/phase0.md §51). The game ships DLSS 310.4 (nvngx_dlss.dll, nvngx_dlssg.dll in the game
// folder), which the NVIDIA driver's NGX core (_nvngx.dll) loads for Streamline 2.9.1. The NVIDIA driver / app keeps
// the newest DLSS in C:\ProgramData\NVIDIA\NGX\models\<feature>\versions\<version>\files\*.bin. With "dlssUpgrade",
// the NGX core's file calls for the game's copies are pointed at those (only files signed by NVIDIA; nothing is copied
// or downloaded). With "dlssPreset", the Super Resolution model the game asks Streamline for is replaced (DLSS 4.5
// models: M for Performance, L for Ultra Performance).
namespace loader::game::dlss
{
	// At startup, after MinHook is initialised.
	void Start();
	// config dlssPreset changed; applies from the next frame.
	void PresetChanged();

	struct Status
	{
		bool checked = false;          // NVIDIA's store has been looked at
		bool upgradeAtStart = false;   // dlssUpgrade when the game started
		std::string gameSr, gameFg;    // versions shipped with the game ("" = none)
		std::string storeSr, storeFg;  // newest in NVIDIA's store ("" = none)
		std::string loadedSr, loadedFg; // what NGX loaded ("" = not yet)
		bool loadedSrFromStore = false, loadedFgFromStore = false;
		int mode = -1;                 // the game's DLSS mode (Streamline sl::DLSSMode), -1 = DLSS not set up yet
		uint32_t gamePreset = 0;       // preset the game asks for in that mode (sl::DLSSPreset)
		uint32_t sentPreset = 0;       // preset sent to Streamline
	};
	Status GetStatus();

	// "K" for sl::DLSSPreset 11, "" for 0 (default).
	std::string PresetLetter(uint32_t preset);
}
