#include "text.hpp"

#include <Windows.h>

#include <algorithm>
#include <atomic>
#include <cstring>
#include <mutex>
#include <unordered_map>

#include "../log.hpp"

namespace loader::game::text
{
	namespace
	{
		// GXT2 layout: "2TXG", u32 count, {u32 hash, u32 offset}[count] sorted by hash, then
		// NUL-terminated UTF-8 strings; offsets are relative to the start of the table.
		constexpr uint32_t kMagic = '2' | 'T' << 8 | 'X' << 16 | 'G' << 24;
		constexpr uint32_t kMaxEntries = 0x100000;

		struct Entry
		{
			uint32_t hash;
			uint32_t offset;
		};

		std::mutex g_mutex;
		const uint8_t* g_table = nullptr;
		std::atomic_bool g_located = false;
		std::unordered_map<uint32_t, size_t> g_capacity; // hash -> bytes available incl. NUL

		const Entry* FindEntry(const uint8_t* table, uint32_t hash)
		{
			const uint32_t count = *reinterpret_cast<const uint32_t*>(table + 4);
			const auto* begin = reinterpret_cast<const Entry*>(table + 8);
			const auto* end = begin + count;
			const auto* it = std::lower_bound(begin, end, hash, [](const Entry& e, uint32_t h) { return e.hash < h; });
			return it != end && it->hash == hash ? it : nullptr;
		}

		// Validates a candidate table header and looks for the anchor; guarded against bad memory.
		bool ProbeTable(const uint8_t* p, uint32_t anchorHash)
		{
			__try
			{
				if (*reinterpret_cast<const uint32_t*>(p) != kMagic)
					return false;
				const uint32_t count = *reinterpret_cast<const uint32_t*>(p + 4);
				if (count == 0 || count > kMaxEntries)
					return false;
				const Entry* e = FindEntry(p, anchorHash);
				return e && e->offset > 8 + count * sizeof(Entry);
			}
			__except (EXCEPTION_EXECUTE_HANDLER)
			{
				return false;
			}
		}

		bool StillValid()
		{
			__try
			{
				return g_table && *reinterpret_cast<const uint32_t*>(g_table) == kMagic;
			}
			__except (EXCEPTION_EXECUTE_HANDLER)
			{
				return false;
			}
		}
	}

	bool Locate(uint32_t anchorHash)
	{
		// Text tables live in the game's heap; they start on a page boundary.
		MEMORY_BASIC_INFORMATION mbi{};
		for (auto* addr = static_cast<uint8_t*>(nullptr);
		     VirtualQuery(addr, &mbi, sizeof(mbi)) == sizeof(mbi);
		     addr = static_cast<uint8_t*>(mbi.BaseAddress) + mbi.RegionSize)
		{
			if (mbi.State != MEM_COMMIT || mbi.Type != MEM_PRIVATE || !(mbi.Protect & (PAGE_READWRITE | PAGE_EXECUTE_READWRITE)) ||
			    (mbi.Protect & PAGE_GUARD))
				continue;
			auto* region = static_cast<const uint8_t*>(mbi.BaseAddress);
			for (size_t off = 0; off + 8 <= mbi.RegionSize; off += 0x1000)
			{
				if (!ProbeTable(region + off, anchorHash))
					continue;
				std::lock_guard lock(g_mutex);
				g_table = region + off;
				g_capacity.clear();
				g_located = true;
				log::Info("text: GXT2 table found ({} entries)", *reinterpret_cast<const uint32_t*>(g_table + 4));
				return true;
			}
		}
		log::Warn("text: GXT2 table not found");
		return false;
	}

	bool Located()
	{
		return g_located;
	}

	bool Replace(uint32_t hash, std::string_view utf8)
	{
		std::lock_guard lock(g_mutex);
		if (!StillValid())
		{
			g_located = false;
			return false;
		}
		const Entry* e = FindEntry(g_table, hash);
		if (!e)
			return false;
		auto* dst = const_cast<char*>(reinterpret_cast<const char*>(g_table + e->offset));

		auto cap = g_capacity.find(hash);
		if (cap == g_capacity.end())
			cap = g_capacity.emplace(hash, std::strlen(dst) + 1).first;

		size_t n = std::min(utf8.size(), cap->second - 1);
		while (n > 0 && n < utf8.size() && (static_cast<uint8_t>(utf8[n]) & 0xC0) == 0x80)
			--n; // never split a multi-byte character
		if (std::strncmp(dst, utf8.data(), n) == 0 && dst[n] == '\0')
			return true;

		std::memcpy(dst, utf8.data(), n);
		dst[n] = '\0';
		return true;
	}
}
