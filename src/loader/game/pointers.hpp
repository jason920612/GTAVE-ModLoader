#pragma once
#include <cstdint>

namespace loader::game
{
	struct scrProgram;
	struct scrThread;

	// Addresses resolved from signatures once the game code is decrypted.
	struct Pointers
	{
		// void InitNativeTables(scrProgram*): turns the hash array of a program into handler pointers.
		void (*InitNativeTables)(scrProgram*) = nullptr;
		// bool RunScriptThreads(int opsToExecute): ticks every script thread once per frame.
		void* RunScriptThreads = nullptr;
		// atArray<scrThread*> { scrThread** data; uint16_t count; uint16_t capacity; }
		scrThread*** ScriptThreads = nullptr;
		uint16_t* ScriptThreadCount = nullptr;
		// _tls_index used by the game's TLS context lookup (gs:[0x58][index]).
		uint32_t* TlsIndex = nullptr;
		// Global copy of the running script thread, written next to the TLS slot.
		scrThread** ActiveThread = nullptr;
	};

	inline Pointers g_pointers;

	// Returns false (and logs which) if any required signature is missing.
	bool ResolvePointers();
}
