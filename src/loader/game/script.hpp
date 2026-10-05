#pragma once
#include <cstdint>

#include "pointers.hpp"

namespace loader::game::script
{
	// joaat, as used for script names.
	constexpr uint32_t Joaat(const char* s)
	{
		uint32_t h = 0;
		for (; *s; ++s)
		{
			char c = *s;
			if (c >= 'A' && c <= 'Z')
				c = static_cast<char>(c - 'A' + 'a');
			h += static_cast<uint8_t>(c);
			h += h << 10;
			h ^= h >> 6;
		}
		h += h << 3;
		h ^= h >> 11;
		h += h << 15;
		return h;
	}

	// Called every frame on the game thread, after the game's own scripts ran.
	using TickCallback = void (*)();

	bool InstallHooks(TickCallback onTick);

	// Returns the running script thread with this name hash, or nullptr.
	scrThread* FindThread(uint32_t nameHash);

	// Makes `thread` the current script thread for the duration of the scope, so natives
	// that need a script context (entity creation, etc.) work from mod code.
	class ScopedThread
	{
	public:
		explicit ScopedThread(scrThread* thread);
		~ScopedThread();
		ScopedThread(const ScopedThread&) = delete;
		ScopedThread& operator=(const ScopedThread&) = delete;

	private:
		uint8_t* m_tls;
		scrThread* m_prevThread;
		scrThread* m_prevGlobal;
		bool m_prevActive;
	};
}
