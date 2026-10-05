#pragma once
#include <Windows.h>

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
	};

	// Scans ModLoader\mods, creates each mod's folder and loads it. Game thread only.
	void LoadAll();
	// Runs every mod's MLMain fiber that is due. Game thread only, inside a script context.
	void Tick();

	const std::vector<std::unique_ptr<Mod>>& All();
	const char* ToString(State state);
}
