#pragma once

// Hang diagnostics: the game's own hang watchdog crashes the game when the main thread's heartbeat stops for 60 s
// (research/phase0.md §24). We watch the same heartbeat and, shortly before the watchdog fires, write a minidump
// with every thread's stack to ModLoader\hangdumps so the hang itself can be analysed.
namespace loader::debug::hangdump
{
	// Starts the watcher thread (once). Call after the game code is decrypted.
	void Start();

	// Seconds the main thread's heartbeat has not changed (0 while it runs or the watchdog is paused). Any thread.
	int StalledSeconds();
}
