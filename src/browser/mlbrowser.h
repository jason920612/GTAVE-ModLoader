/*
 * Interface between ModLoader.dll and ModLoader\browser\mlbrowser.dll (the in-game web browser, built on CEF).
 * ModLoader.dll loads mlbrowser.dll the first time the browser is opened and talks to it only through these exports.
 *
 * The browser shows the game's own web pages: every http / https request is answered from the web roots
 * (<root>\<host>\<path>, "index.html" for a folder); nothing goes to the internet.
 * Pages call into the game with  game.call(name, ...args)  -> Promise  and listen with  game.on(event, fn).
 */
#pragma once
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct MLBCallbacks
{
	/* A page called game.call(): `request` is JSON {"name": ..., "args": [...]}. Answer with MLB_Respond(id, ...).
	 * Runs on a browser thread. */
	void (*query)(int64_t id, const char* request);
	/* Messages for the loader log (0 debug .. 3 error). Any thread. */
	void (*log)(int level, const char* text);
	/* A page wants https://gametextures/<dictionary>/<texture>.png: answer with MLB_TextureReady(id, png file). */
	void (*texture)(int64_t id, const char* dictionary, const char* texture);
} MLBCallbacks;

typedef struct MLBConfig
{
	const wchar_t* browserDir; /* ModLoader\browser: libcef.dll, resources, ml_browser_helper.exe */
	const wchar_t* cacheDir;   /* profile / cache folder */
	const wchar_t* const* webRoots; /* folders holding <host>\... page trees, searched in order */
	int webRootCount;
	const char* locale; /* e.g. "zh-TW" */
	MLBCallbacks callbacks;
} MLBConfig;

/* Starts CEF (once). Returns 1 on success. */
typedef int (*MLB_InitFn)(const MLBConfig* config);
/* Shows the browser at `url` with a view of width x height pixels (creates it on first use). */
typedef void (*MLB_OpenFn)(const char* url, int width, int height);
/* Hides the browser (it stays loaded). */
typedef void (*MLB_HideFn)(void);
/* Copies the newest frame into `dst` (BGRA, `pitch` bytes per row, at most width x height) when it is newer than
 * *serial; returns 1 and updates *serial when it copied. Any thread. */
typedef int (*MLB_FrameFn)(uint32_t* serial, void* dst, int pitch, int width, int height);
/* Input, in view pixels. button: 0 left, 1 middle, 2 right. */
typedef void (*MLB_MouseMoveFn)(int x, int y);
typedef void (*MLB_MouseButtonFn)(int x, int y, int button, int down);
typedef void (*MLB_WheelFn)(int x, int y, int dx, int dy);
/* A Windows keyboard message (WM_KEYDOWN, WM_KEYUP, WM_CHAR, WM_SYSKEYDOWN, ...). */
typedef void (*MLB_KeyFn)(unsigned msg, uint64_t wparam, int64_t lparam);
/* Back in the history; returns 0 when there is no page to go back to. */
typedef int (*MLB_BackFn)(void);
/* Forward in the history; returns 0 when there is none. */
typedef int (*MLB_ForwardFn)(void);
/* The current address (UTF-8) into buf; returns its length. */
typedef int (*MLB_UrlFn)(char* buf, int size);
/* Answers a game.call(): ok = 1 resolves the promise with `result` (JSON text, NULL = null), 0 rejects with it. */
typedef void (*MLB_RespondFn)(int64_t id, int ok, const char* result);
/* Sends an event to the page: listeners of game.on(event) get `json` parsed (NULL = null). */
typedef void (*MLB_EmitFn)(const char* event, const char* json);
/* The PNG file for a texture request (UTF-8 path; NULL or "" = not found). Any thread. */
typedef void (*MLB_TextureReadyFn)(int64_t id, const char* file);
/* Stops CEF (at game exit). */
typedef void (*MLB_ShutdownFn)(void);

#ifdef __cplusplus
}
#endif
