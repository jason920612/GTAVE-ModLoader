#pragma once
#include <cstdint>

namespace loader::game
{
	struct scrProgram;
	struct scrThread;

	// Layout read by SetRouterLink (RVA 0x13C4F30 on build 0x6aa45f10).
	struct ScriptRouterLink
	{
		int32_t source;  // SRCS_*
		int32_t _pad0;
		int32_t mode;    // SRCM_*
		int32_t _pad1;
		int32_t argType; // SRCA_*
		int32_t _pad2;
		char arg[0x80];
	};

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
		// Gen9 Script Router: the landing page acts on a pending "source=..,mode=..,argType=..,arg=.."
		// request. SetRouterLink builds it from a ScriptRouterLink; ClearRouterLink drops it.
		void (*SetRouterLink)(const ScriptRouterLink* link) = nullptr;
		void (*ClearRouterLink)() = nullptr;
		struct AtString
		{
			const char* data;
			uint16_t length;
			uint16_t capacity;
		}* RouterLink = nullptr;
	};

	inline Pointers g_pointers;

	// Returns false (and logs which) if any required signature is missing.
	bool ResolvePointers();
}
