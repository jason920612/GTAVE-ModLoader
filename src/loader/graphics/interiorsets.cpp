#include "interiorsets.hpp"

#include <Windows.h>

#include <algorithm>
#include <cmath>
#include <cstring>
#include <vector>

#include "../log.hpp"
#include "../pattern.hpp"

namespace loader::graphics::interiorsets
{
	namespace
	{
		// The encrypted pool pointer: two 64-bit words and a flag byte (bit 0: there is a pool).
		const uint64_t* g_wordA = nullptr;
		const uint64_t* g_wordB = nullptr;
		const uint8_t* g_flag = nullptr;
		bool g_tried = false;

		uint64_t Rol(uint64_t v, unsigned n)
		{
			n &= 63;
			return n ? (v << n) | (v >> (64 - n)) : v;
		}

		// As the game decodes it: a = rol(A, 28); p = rol(rol(B ^ a, 32), (a & 31) + 3), inverted.
		uintptr_t Pool()
		{
			if (!g_flag || !(*g_flag & 1))
				return 0;
			const uint64_t a = Rol(*g_wordA, 0x1C);
			const uint64_t p = Rol(Rol(*g_wordB ^ a, 0x20), static_cast<unsigned>((a & 0x1F) + 3));
			return static_cast<uintptr_t>(~p);
		}

		// (memory of another subsystem's: read under SEH, as a pool slot can be in flux)
		bool Copy(void* to, uintptr_t from, size_t size)
		{
			__try
			{
				std::memcpy(to, reinterpret_cast<const void*>(from), size);
				return true;
			}
			__except (EXCEPTION_EXECUTE_HANDLER)
			{
				return false;
			}
		}
	}

	bool Install()
	{
		if (g_tried)
			return g_flag != nullptr;
		g_tried = true;
		// IS_INTERIOR_ENTITY_SET_ACTIVE's work: test ecx,ecx; jle; ...; movzx ecx,[flag]; mov rcx,[A]; rol rcx,1Ch;
		// mov rbx,[B]; xor rbx,rcx; rol rbx,20h; ...; movzx ecx,[flag]
		const auto module = pattern::Module::Main();
		const auto at = pattern::Find(module.text,
		    pattern::Pattern::Parse("56 57 53 48 83 EC 30 85 C9 0F 8E ? ? ? ? 48 89 D6 89 CA 89 C8 0F B6 0D ? ? ? ? 48 8B 0D ? ? ? ? 48 C1 C1 1C "
		                            "48 8B 1D ? ? ? ? 48 31 CB 48 C1 C3 20 C1 E8 08 80 E1 1F 80 C1 03 48 D3 C3 48 F7 D3 0F B6 0D ? ? ? ?"));
		if (!at)
		{
			log::Warn("ue5 interiors: the game's interior pool not found; interiors show their default entity sets");
			return false;
		}
		g_wordA = reinterpret_cast<const uint64_t*>(pattern::Rip(*at + 32));
		g_wordB = reinterpret_cast<const uint64_t*>(pattern::Rip(*at + 43));
		g_flag = reinterpret_cast<const uint8_t*>(pattern::Rip(*at + 72));
		log::Info("ue5 interiors: the interior pool's key at +{:#x}", reinterpret_cast<uintptr_t>(g_wordA) - module.base);
		return true;
	}

	size_t Near(const float point[3], float radius, GTAVBridge::InteriorSets* out, size_t max)
	{
		const uintptr_t pool = Pool();
		if (!pool)
			return 0;
		// The pool: +0x08 the slots, +0x10 a byte a slot (0x80: free), +0x18 how many, +0x1C a slot's size.
		uintptr_t storage = 0, flags = 0;
		int32_t size = 0, item = 0;
		if (!Copy(&storage, pool + 0x08, 8) || !Copy(&flags, pool + 0x10, 8) || !Copy(&size, pool + 0x18, 4) || !Copy(&item, pool + 0x1C, 4) ||
		    size <= 0 || size > 100000 || item < 0xD8 || item > 0x1000)
			return 0;
		std::vector<uint8_t> used(static_cast<size_t>(size));
		if (!Copy(used.data(), flags, used.size()))
			return 0;
		struct Found
		{
			float distance;
			GTAVBridge::InteriorSets sets;
		};
		std::vector<Found> found;
		for (int32_t i = 0; i < size; ++i)
		{
			if (used[static_cast<size_t>(i)] & 0x80)
				continue;
			const uintptr_t proxy = storage + static_cast<uintptr_t>(i) * static_cast<uintptr_t>(item);
			float position[3];
			if (!Copy(position, proxy + 0x60, sizeof(position)))
				continue;
			const float d = std::hypot(position[0] - point[0], position[1] - point[1]);
			if (!(d <= radius))
				continue;
			Found f{d, {}};
			std::memcpy(f.sets.position, position, sizeof(position));
			uintptr_t entries = 0;
			uint16_t count = 0;
			if (!Copy(&f.sets.name, proxy + 0xD4, 4) || !Copy(&entries, proxy + 0xC0, 8) || !Copy(&count, proxy + 0xC8, 2))
				continue;
			f.sets.count = 0;
			for (uint16_t k = 0; k < count && f.sets.count < GTAVBridge::kMaxInteriorSets && entries; ++k)
				if (uint32_t name = 0; Copy(&name, entries + 12u * k, 4))
					f.sets.sets[f.sets.count++] = name;
			found.push_back(f);
		}
		std::sort(found.begin(), found.end(), [](const Found& a, const Found& b) { return a.distance < b.distance; });
		const size_t n = std::min(found.size(), max);
		for (size_t k = 0; k < n; ++k)
			out[k] = found[k].sets;
		return n;
	}
}
