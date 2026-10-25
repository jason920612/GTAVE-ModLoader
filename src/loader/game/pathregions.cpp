#include "pathregions.hpp"

#include <Windows.h>

#include <cstring>

#include "../config.hpp"
#include "../log.hpp"
#include "../pattern.hpp"

namespace loader::game::pathregions
{
	namespace
	{
#include "pathregions_data.inc"

		uint8_t* g_block = nullptr;

		// Executable memory within 2 GB of `target` (the patches jump to it with rel32).
		uint8_t* AllocateNear(uintptr_t target, size_t size)
		{
			SYSTEM_INFO info{};
			GetSystemInfo(&info);
			const uintptr_t granularity = info.dwAllocationGranularity;
			const uintptr_t start = target & ~(granularity - 1);
			for (uintptr_t step = granularity; step < 0x7FF00000; step += granularity)
				for (const uintptr_t at : {start - step, start + step})
					if (void* p = VirtualAlloc(reinterpret_cast<void*>(at), size, MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE))
						return static_cast<uint8_t*>(p);
			return nullptr;
		}

		void Write(uintptr_t at, const uint8_t* bytes, size_t size)
		{
			DWORD old;
			VirtualProtect(reinterpret_cast<void*>(at), size, PAGE_EXECUTE_READWRITE, &old);
			std::memcpy(reinterpret_cast<void*>(at), bytes, size);
			VirtualProtect(reinterpret_cast<void*>(at), size, old, &old);
			FlushInstructionCache(GetCurrentProcess(), reinterpret_cast<void*>(at), size);
		}
	}

	bool Install()
	{
		if (!config::Get().worldPathRegions || g_block)
			return g_block != nullptr;
		const auto module = pattern::Module::Main();
		// the build must be the one the stubs were made from: every byte they replace as expected
		for (const PathSite& s : kPathSites)
			if (std::memcmp(reinterpret_cast<const void*>(module.base + s.rva), s.original, s.length) != 0)
			{
				log::Warn("world path regions: code at {:#x} differs from the build the patches were made for; not patched", s.rva);
				return false;
			}
		for (const PathRewrite& w : kPathRewrites)
			if (std::memcmp(reinterpret_cast<const void*>(module.base + w.rva), w.original, w.length) != 0)
			{
				log::Warn("world path regions: code at {:#x} differs from the build the patches were made for; not patched", w.rva);
				return false;
			}

		const bool grid = config::Get().worldPathGrid;
		if (grid)
			for (const PathRedirect& r : kPathGrid)
				if (std::memcmp(reinterpret_cast<const void*>(module.base + r.rva), r.original, r.length) != 0)
				{
					log::Warn("world path regions: code at {:#x} differs from the build the patches were made for; not patched", r.rva);
					return false;
				}

		// the grid's constants after the stubs, 16-byte aligned
		const size_t constants = (kPathStubs + sizeof(kPathStubBytes) + 15) & ~size_t(15);
		const size_t size = constants + sizeof(kPathGridConstants);
		g_block = AllocateNear(module.base, size);
		if (!g_block)
		{
			log::Error("world path regions: no memory within 2 GB of the game");
			return false;
		}
		// tables zeroed (every region at its own place), the stubs after them
		std::memcpy(g_block + kPathStubs, kPathStubBytes, sizeof(kPathStubBytes));
		for (const PathReloc& r : kPathRelocs)
		{
			const int64_t rel = static_cast<int64_t>(module.base + r.target) - static_cast<int64_t>(reinterpret_cast<uintptr_t>(g_block) + r.end);
			const int32_t rel32 = static_cast<int32_t>(rel);
			std::memcpy(g_block + r.at, &rel32, 4);
		}
		FlushInstructionCache(GetCurrentProcess(), g_block, size);

		for (const PathSite& s : kPathSites)
		{
			uint8_t jump[32];
			std::memset(jump, 0x90, sizeof(jump));
			jump[0] = 0xE9;
			const int32_t rel = static_cast<int32_t>(reinterpret_cast<intptr_t>(g_block + s.stub) - static_cast<intptr_t>(module.base + s.rva + 5));
			std::memcpy(jump + 1, &rel, 4);
			Write(module.base + s.rva, jump, s.length);
		}
		for (const PathRewrite& w : kPathRewrites)
			Write(module.base + w.rva, w.replacement, w.length);
		log::Info("world path regions: {} reads of node/junction positions patched ({} stub bytes at {})", std::size(kPathSites),
		    sizeof(kPathStubBytes), static_cast<void*>(g_block));
		if (grid)
		{
			// 1024 m regions from -16384 (32 x 32 as before): the path code's grid constants read from scaled copies
			std::memcpy(g_block + constants, kPathGridConstants, sizeof(kPathGridConstants));
			for (const PathRedirect& r : kPathGrid)
			{
				uint8_t bytes[16];
				std::memcpy(bytes, r.original, r.length);
				const int32_t disp = static_cast<int32_t>(reinterpret_cast<intptr_t>(g_block + constants + r.constant * 16) -
				                                          static_cast<intptr_t>(module.base + r.rva + r.length));
				std::memcpy(bytes + r.dispOffset, &disp, 4);
				Write(module.base + r.rva, bytes, r.length);
			}
			log::Info("world path regions: region grid of 1024 m ({} constant reads moved)", std::size(kPathGrid));
		}
		return true;
	}

	bool Installed()
	{
		return g_block != nullptr;
	}

	void SetOrigin(uint16_t area, int32_t x, int32_t y)
	{
		if (!g_block)
			return;
		auto* t16 = reinterpret_cast<int32_t*>(g_block + kPathTable16) + area * 4;
		auto* t8 = reinterpret_cast<int32_t*>(g_block + kPathTable8) + area * 2;
		t16[0] = x, t16[1] = y;
		t8[0] = x, t8[1] = y;
	}
}
