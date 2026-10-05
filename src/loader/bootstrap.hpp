#pragma once

namespace loader::bootstrap
{
	using Callback = void (*)();

	// Called from DllMain. Hooks an early CRT-time API so we regain control right after
	// Arxan has decrypted the game code but before the game mounts its file system;
	// `onUnpacked` then runs once on that thread.
	bool Install(Callback onUnpacked);
}
