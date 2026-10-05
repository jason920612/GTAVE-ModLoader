#pragma once
#include <Windows.h>

#include <atomic>

#include <cstdio>
#include <filesystem>
#include <memory>
#include <string>
#include <vector>

#include <modloader/modloader.h>

#include "game/natives.hpp"

namespace loader::mods
{
	enum class State
	{
		Disabled, // turned off in loader.json
		Failed,   // could not be loaded (see error)
		Loaded,   // loaded, no MLMain or waiting for story mode
		Running,  // MLMain is scheduled every frame
		Finished, // MLMain returned
		Faulted,  // crashed; never scheduled again
	};

	struct Mod;

	// A value registered with MLApi::AddSetting, shown on the mod's pause menu page.
	struct Setting
	{
		Mod* owner = nullptr;
		std::string id, label;
		MLSettingType type = ML_SETTING_TOGGLE;
		int32_t defaultValue = 0;
		std::atomic<int32_t> value = 0;

		int32_t Max() const { return type == ML_SETTING_SLIDER ? 10 : 1; }
	};

	struct Mod
	{
		std::filesystem::path file; // ModLoader\mods\<name>.dll
		std::string fileName;       // key used in loader.json
		std::string name, version, author, description;
		std::filesystem::path dir;  // per-mod folder: ModLoader\mods\<stem>
		std::wstring dirStr, configStr, dataStr;
		MLContext context{};

		HMODULE module = nullptr;
		MLMainFn main = nullptr;
		MLOnUnloadFn onUnload = nullptr;

		State state = State::Loaded;
		std::string error;

		void* fiber = nullptr;
		uint64_t wakeAt = 0;
		game::natives::Invocation invocation;
		FILE* log = nullptr;

		std::vector<Setting*> settings; // in registration order; fixed once MLOnLoad returned
	};

	// Scans ModLoader\mods, creates each mod's folder and loads it. Game thread only.
	void LoadAll();
	// Runs every mod's MLMain fiber that is due. Game thread only, inside a script context.
	void Tick();

	const std::vector<std::unique_ptr<Mod>>& All();

	// Thread-safe copy of what the UI needs.
	struct ModView
	{
		std::string fileName, name, version, author, description, error;
		std::filesystem::path dir;
		State state;
	};
	std::vector<ModView> Snapshot();
	bool Loaded(); // LoadAll has run
	const char* ToString(State state);

	// Stores a value changed in the pause menu and saves the owner's settings.json. Game thread.
	void SetSettingValue(Setting& setting, int32_t value);
}
