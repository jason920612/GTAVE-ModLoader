#pragma once
#include <Windows.h>

#include <atomic>

#include <cstdio>
#include <filesystem>
#include <memory>
#include <string>
#include <vector>

#include <modloader/modloader.h>
#include <nlohmann/json.hpp>

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

	enum class ItemKind : uint8_t
	{
		Page,
		Toggle,
		Number,
		List,
		Action,
		Text,
		Hotkey,
	};

	// A menu entry registered through MLApi (settings are items too). Handle = index; never reused.
	// Guarded by the menu mutex, except `value`.
	struct Item
	{
		Mod* owner = nullptr;
		int32_t parent = ML_ROOT_PAGE;
		ItemKind kind = ItemKind::Text;
		bool removed = false;
		bool enabled = true;
		bool pauseMenu = false; // registered in MLOnLoad and representable there (toggle, list, 0..10 whole number)
		std::string id, label;
		float min = 0, max = 1, step = 1;
		std::atomic<float> value = 0;
		std::vector<std::string> options; // lists
		MLCallback fn = nullptr;          // actions and hotkeys: activation; others: value changed
		void* user = nullptr;
		std::vector<int32_t> children; // pages

		int32_t IntValue() const { return static_cast<int32_t>(value.load()); }
	};

	// A mod's fiber: MLMain, or the one that runs menu callbacks.
	struct Task
	{
		void* fiber = nullptr;
		uint64_t wakeAt = 0;
		bool busy = false; // callback task: has work (running or waiting)
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

		Task mainTask, callbackTask;
		game::natives::Invocation invocation;
		FILE* log = nullptr;

		// Menu (menu mutex): root items in order; pages keep their own children.
		std::vector<int32_t> rootItems;
		std::vector<std::pair<MLCallback, void*>> pending; // callbacks waiting for the callback task
		bool settingsDirty = false;
		nlohmann::json saved; // settings.json as read (and as written last)
		std::vector<Item*> pauseItems; // fixed once MLOnLoad returned
	};

	// Scans ModLoader\mods, creates each mod's folder and loads it. Game thread only.
	void LoadAll();
	// Runs every mod's fibers that are due. Game thread only, inside a script context.
	void Tick();

	const std::vector<std::unique_ptr<Mod>>& All();

	// Thread-safe copy of what the UI needs.
	struct ModView
	{
		std::string fileName, name, version, author, description, error;
		std::filesystem::path dir;
		State state;
		bool hasMenu = false;
	};
	std::vector<ModView> Snapshot();
	bool Loaded(); // LoadAll has run
	const char* ToString(State state);

	// Stores a value changed in the pause menu and saves the owner's settings.json. Game thread.
	void SetSettingValue(Item& item, int32_t value);

	// ---- loader window (any thread) ----
	struct ItemView
	{
		int32_t handle;
		ItemKind kind;
		bool enabled;
		std::string label;
		float value, min, max, step;
		std::vector<std::string> options;
	};
	// Items of a page (ML_ROOT_PAGE = the mod's root), in order. `mod` indexes All().
	std::vector<ItemView> PageItems(size_t mod, int32_t page);
	bool PageExists(size_t mod, int32_t page);
	// The player changed a value / activated an action.
	void UiSetValue(int32_t handle, float value);
	void UiActivate(int32_t handle);
	// A key went down in the game window while the loader UI is closed.
	void OnKeyDown(uint32_t vk);
}
