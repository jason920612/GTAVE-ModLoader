#pragma once

namespace loader::core
{
	// Kicks off work that can overlap with game startup (crossmap download).
	void StartBackgroundTasks();

	// Runs once the game code is decrypted (early, on the main thread, before file mounting).
	void OnGameUnpacked();
}
