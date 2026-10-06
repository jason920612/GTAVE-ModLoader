/*
 * GTAV Enhanced ModLoader SDK - C ABI.
 *
 * A mod is a DLL placed in <game>\ModLoader\mods\. It must export:
 *   const MLModInfo* MLGetModInfo(void);
 *   int  MLOnLoad(const MLApi* api, const MLContext* ctx);   // return 0 to cancel loading
 * and may export:
 *   void MLMain(void);     // runs as a game script; loop forever and call api->Wait()
 *   void MLOnUnload(void); // called when the game exits
 *
 * Settings registered in MLOnLoad appear on the mod's page in the pause menu (story mode,
 * "Mods" tab) and are saved to <modDir>\settings.json.
 *
 * Menus (API additions after the first release, see AddPage and below) are shown in the loader
 * window (F4 by default, tab "模組功能"): pages, toggles, numbers, lists, actions and hotkeys.
 * Toggles, lists and 0..10 whole-number items registered in MLOnLoad also appear in the pause menu
 * (at most ML_MAX_SETTINGS per mod there; the rest only in the loader window).
 *
 * Natives may only be called from MLMain (it runs on the game's script thread).
 * MLOnLoad runs before the world exists: use it for setup, not for natives.
 */
#pragma once
#include <stdint.h>
#include <wchar.h>

#define ML_API_VERSION 1

#ifdef __cplusplus
extern "C" {
#endif

typedef struct MLModInfo
{
	uint32_t apiVersion; /* always ML_API_VERSION */
	const char* name;
	const char* version;
	const char* author;
	const char* description;
} MLModInfo;

typedef struct MLContext
{
	const wchar_t* modDir;     /* <game>\ModLoader\mods\<ModName>\ (created by the loader) */
	const wchar_t* configPath; /* <modDir>\config.json (may not exist yet) */
	const wchar_t* dataDir;    /* <modDir>\data\ (created by the loader) */
} MLContext;

typedef enum MLLogLevel
{
	ML_LOG_DEBUG = 0,
	ML_LOG_INFO = 1,
	ML_LOG_WARN = 2,
	ML_LOG_ERROR = 3,
} MLLogLevel;

typedef enum MLSettingType
{
	ML_SETTING_TOGGLE = 0, /* value 0 (off) or 1 (on) */
	ML_SETTING_SLIDER = 1, /* value 0..10 */
	ML_SETTING_LIST = 2,   /* value 0..count-1, an index into the option texts (AddListSetting) */
} MLSettingType;

#define ML_MAX_SETTINGS 15     /* per mod in the pause menu */
#define ML_MAX_LIST_OPTIONS 32 /* per list setting */

/* Menu callbacks run on the mod's own script fiber (natives and Wait allowed), one at a time,
 * in the order they were triggered. They also run for mods without MLMain. */
typedef void (*MLCallback)(void* user);

#define ML_ROOT_PAGE (-1) /* the mod's own top-level page */

typedef enum MLModelType
{
	ML_MODEL_VEHICLE = 0,
	ML_MODEL_PED = 1,
} MLModelType;

/* `name` is the model name ("" when unknown); `pack` is the ModLoader\mods pack folder it comes from,
 * "" for the game's own models. */
typedef void (*MLModelVisitor)(uint32_t hash, const char* name, const char* pack, void* user);

typedef struct MLApi
{
	uint32_t apiVersion;
	uint32_t size; /* sizeof(MLApi) as built by the loader; newer fields are appended */

	/* Native invocation, using the public (nativedb) hashes. Begin -> Push* -> Call. */
	void (*NativeBegin)(uint64_t hash);
	void (*NativePush)(uint64_t value);
	uint64_t* (*NativeCall)(void); /* returns the result slots; valid until the next call */

	/* Yield to the game for at least `ms` milliseconds (0 = next frame). MLMain only. */
	void (*Wait)(uint32_t ms);

	/* Writes to the mod's own log.txt and to the loader log. */
	void (*Log)(MLLogLevel level, const char* message);

	/* Milliseconds since the loader started. */
	uint64_t (*GetTickMs)(void);

	/* ---- added after the first release: check `size` before use (see modloader.hpp) ---- */

	/* Adds a setting to the mod's pause menu page. MLOnLoad only. `id` is the key in
	 * settings.json (ASCII), `label` is shown in the menu (UTF-8). The saved value, if any,
	 * replaces `defaultValue`. Returns a handle for GetSetting, or -1 on error. */
	int32_t (*AddSetting)(MLSettingType type, const char* id, const char* label, int32_t defaultValue);
	/* Current value of a setting. Any thread. */
	int32_t (*GetSetting)(int32_t handle);
	/* Like AddSetting with ML_SETTING_LIST: the player picks one of `count` texts (UTF-8,
	 * 2..ML_MAX_LIST_OPTIONS). The value is the index of the chosen text. MLOnLoad only. */
	int32_t (*AddListSetting)(const char* id, const char* label, const char* const* options, int32_t count, int32_t defaultValue);

	/* ---- menus: check `size` before use (see modloader.hpp) ----
	 * Game thread only: MLOnLoad, MLMain or a menu callback. Returns an item handle, or -1.
	 * `page` is ML_ROOT_PAGE or a handle from AddPage. `id` (ASCII) keys the value in settings.json;
	 * NULL or "" = not saved. Labels are UTF-8. Items keep the order they were added in. */
	int32_t (*AddPage)(int32_t page, const char* label);
	int32_t (*AddToggle)(int32_t page, const char* id, const char* label, int32_t defaultValue);
	/* min..max in steps of `step` (> 0; a whole number step shows whole numbers). */
	int32_t (*AddNumber)(int32_t page, const char* id, const char* label, float min, float max, float step, float defaultValue);
	/* 2..ML_MAX_LIST_OPTIONS texts; the value is the chosen index. */
	int32_t (*AddList)(int32_t page, const char* id, const char* label, const char* const* options, int32_t count, int32_t defaultValue);
	/* A button: `fn(user)` runs when the player activates it. */
	int32_t (*AddAction)(int32_t page, const char* label, MLCallback fn, void* user);
	/* A line of text (e.g. status). */
	int32_t (*AddText)(int32_t page, const char* label);
	/* A key the player can rebind in the loader window; `fn(user)` runs when it is pressed in story mode
	 * while the loader window is closed. `defaultKey` is a Windows virtual-key code (0 = unbound).
	 * The value is the bound key. Listed on the mod's root page. */
	int32_t (*AddHotkey)(const char* id, const char* label, uint32_t defaultKey, MLCallback fn, void* user);

	/* Runs `fn(user)` when the player changes the item's value (toggles, numbers, lists, hotkeys). */
	void (*SetCallback)(int32_t item, MLCallback fn, void* user);
	void (*SetLabel)(int32_t item, const char* label);
	void (*SetEnabled)(int32_t item, int32_t enabled);
	/* Removes every item on a page (their handles become invalid); the page itself stays. */
	void (*ClearPage)(int32_t page);
	/* Value of any item, from any thread (toggle 0/1, list index, number, hotkey key code). */
	float (*GetValue)(int32_t item);
	/* Sets a value (clamped and snapped to the item's range); no callback runs. */
	void (*SetValue)(int32_t item, float value);

	/* Shows a short message on screen for a few seconds (UTF-8). Any thread. */
	void (*Notify)(const char* text);

	/* ---- models: check `size` before use ----
	 * Calls `fn` for every vehicle or ped model the game has registered, add-on packs included.
	 * MLMain or a callback only. Returns the number of models, or -1 while the loader is still reading the
	 * model names (shortly after the game starts; try again later). */
	int32_t (*EnumModels)(MLModelType type, MLModelVisitor fn, void* user);
} MLApi;

typedef const MLModInfo* (*MLGetModInfoFn)(void);
typedef int (*MLOnLoadFn)(const MLApi* api, const MLContext* ctx);
typedef void (*MLMainFn)(void);
typedef void (*MLOnUnloadFn)(void);

#ifdef __cplusplus
}
#endif
