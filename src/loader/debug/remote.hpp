#pragma once
#include <cstdint>

// Test control (config debugRemote): a tester drives the game through a named pipe without the game window being in
// front or even on screen, so the person at the computer keeps using it. Keys reach the mods' hotkeys and controls
// reach the game from inside (nothing goes through the system's input), screenshots come from the game's own frames.
// Client: tools/gtactl.py. Protocol: one command per line, one reply line ("ok ..." or "err ...").
//   ping                      the game's state (landing / story / online)
//   story                     continues story mode from the home screen
//   key <vk>                  a mod hotkey press (F9, Q, 0x78 ...)
//   gamekey <vk> [ms]         a key press for the game itself (raw input), frontend menus included
//   control <id> [ms] [value] a game control held for ms (default 120) at value (default 1)
//   release                   lets go of every held control
//   shot <path>               the next frame as a PNG
//   window offscreen|restore  moves the game window out of view (or back), the keyboard back to the person's window
//   call <name> [json args]   a mod's web function (RegisterWebFunction) as a page's game.call runs it; its result
//   mute on|off               the game's sound (its entry in the volume mixer); Windows keeps it for the next start,
//                             so a test's mute is undone at the next start without test control too
namespace loader::debug::remote
{
	// A mod's web function called by the tester ("call", negative ids) answered (mods.cpp). Any thread.
	void ModCallAnswered(int64_t id, bool ok, const char* result);

	// Starts the pipe server when enabled.
	void Start();
	// Game thread, script context, every tick: the held controls.
	void Tick();
	// The window procedure: a key a tester sent (posted to the game window, handled on its thread).
	constexpr uint32_t kKeyMessage = 0x8000 + 0x4D4C; // WM_APP + 'ML'
}
