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
} MLSettingType;

#define ML_MAX_SETTINGS 15 /* per mod */

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
} MLApi;

typedef const MLModInfo* (*MLGetModInfoFn)(void);
typedef int (*MLOnLoadFn)(const MLApi* api, const MLContext* ctx);
typedef void (*MLMainFn)(void);
typedef void (*MLOnUnloadFn)(void);

#ifdef __cplusplus
}
#endif
