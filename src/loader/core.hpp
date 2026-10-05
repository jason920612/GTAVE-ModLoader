#pragma once

namespace loader::core
{
	// Runs once the game code is decrypted (early, on the main thread, before file mounting).
	void OnGameUnpacked();
}
