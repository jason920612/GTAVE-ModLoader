#pragma once

namespace loader::bootstrap
{
	// Called from DllMain. Hooks an early CRT-time API so we regain control right after
	// Arxan has decrypted the game code but before the game mounts its file system.
	bool Install();
}
