#include "text_override.hpp"

#include <Windows.h>

#include <atomic>
#include <deque>
#include <mutex>
#include <string>

#include "../log.hpp"
#include "../pattern.hpp"

namespace loader::game::text_override
{
	namespace
	{
		// Layout verified against GetByHash (RVA 0x526B90 on build 0x6aa45f10).
		struct Entry
		{
			uint32_t hash;
			uint32_t pad;
			const char* text;
		};
		struct Map
		{
			Entry* entries;    // +0x270
			uint32_t capacity; // +0x278, power of two
			uint32_t count;    // +0x27C
		};

		uint8_t* g_theText = nullptr;
		std::mutex g_mutex;
		std::deque<std::string> g_strings; // stable storage for every string we hand out

		Map* GetMap()
		{
			return g_theText ? reinterpret_cast<Map*>(g_theText + 0x270) : nullptr;
		}

		uint32_t Mix(uint32_t h) // murmur3 fmix32, as used by the game
		{
			h ^= h >> 16;
			h *= 0x85EBCA6B;
			h ^= h >> 13;
			h *= 0xC2B2AE35;
			h ^= h >> 16;
			return h;
		}
	}

	bool Init()
	{
		// lea rcx, TheText; mov edx, edi; call DoesTextLabelExist; test al, al; je ..; lea rcx, ...
		const auto at = pattern::Find(pattern::Module::Main().text, pattern::Pattern::Parse("48 8D 0D ? ? ? ? 89 FA E8 ? ? ? ? 84 C0 74 ? 48 8D 0D"));
		if (!at)
		{
			log::Error("text: TheText not found");
			return false;
		}
		g_theText = reinterpret_cast<uint8_t*>(pattern::Rip(*at + 3));
		return true;
	}

	bool Ready()
	{
		const Map* m = GetMap();
		return m && m->entries && m->capacity && (m->capacity & (m->capacity - 1)) == 0 && m->count < m->capacity;
	}

	bool Set(uint32_t hash, std::string_view utf8)
	{
		if (!Ready() || hash == 0)
			return false;
		std::lock_guard lock(g_mutex);
		Map* m = GetMap();
		if (m->count + 1 >= m->capacity - m->capacity / 8)
		{
			log::Warn("text: override map is too full ({}/{}), not adding more labels", m->count, m->capacity);
			return false;
		}
		const char* str = g_strings.emplace_back(utf8).c_str();

		uint32_t i = Mix(hash) & (m->capacity - 1);
		while (m->entries[i].hash != 0 && m->entries[i].hash != hash)
			i = (i + 1) & (m->capacity - 1);
		Entry& e = m->entries[i];
		const bool added = e.hash == 0;
		// Publish the string before the hash so a concurrent lookup never sees a half entry.
		InterlockedExchangePointer(reinterpret_cast<void* volatile*>(const_cast<char**>(&e.text)), const_cast<char*>(str));
		InterlockedExchange(reinterpret_cast<volatile LONG*>(&e.hash), static_cast<LONG>(hash));
		if (added)
			++m->count;
		return true;
	}
}
