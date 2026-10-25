#include "navgrid.hpp"

#include <Windows.h>

#include <algorithm>
#include <atomic>
#include <cfloat>
#include <cmath>
#include <cstring>

#include <MinHook.h>

#include "../config.hpp"
#include "../log.hpp"
#include "../pattern.hpp"
#include "pools.hpp"

namespace loader::game::navgrid
{
	namespace
	{
#include "navgrid_data.inc"

		// The squares' count in the game's own grid, and what the static copies of it hold (sectors per navmesh, W, H).
		constexpr uint32_t kOldWidth = 100;
		constexpr uintptr_t kStaticSize = 0x28BB37C;
		// CNavMesh's placement in memory (the resource fixup): push rsi; push rdi; push rbx; sub rsp,20h; mov rdi,rdx;
		// mov rsi,rcx; call ...
		constexpr uintptr_t kPlace = 0xCD5BD0;
		constexpr uint8_t kPlaceBytes[] = {0x56, 0x57, 0x53, 0x48, 0x83, 0xEC, 0x20, 0x48, 0x89, 0xD7, 0x48, 0x89, 0xCE, 0xE8};

		using PlaceFn = void* (*)(uint8_t* navmesh, void* resource);
		PlaceFn g_place = nullptr;
		std::atomic<uint32_t> g_renumbered = 0;
		std::atomic<bool> g_warnedLayout = false;

		uint8_t* AllocateNear(uintptr_t target, size_t size)
		{
			SYSTEM_INFO info{};
			GetSystemInfo(&info);
			const uintptr_t granularity = info.dwAllocationGranularity;
			const uintptr_t start = target & ~(granularity - 1);
			for (uintptr_t step = granularity; step < 0x7FF00000; step += granularity)
				for (const uintptr_t at : {start - step, start + step})
					if (void* p = VirtualAlloc(reinterpret_cast<void*>(at), size, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE))
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

		// A split array (+0x08 its element count, +0x10 its chunks {pointer, count}, +0x20 their count) whose chunks hold
		// `per` elements each (the last one the rest), as the game's files have them; Open fails when it is laid out otherwise.
		struct Split
		{
			uint8_t* const* chunks = nullptr;
			uint32_t per = 0, size = 0, count = 0;

			bool Open(const uint8_t* object, uint32_t elementsPerChunk, uint32_t elementSize)
			{
				if (!object)
					return false;
				const uint8_t* list = *reinterpret_cast<uint8_t* const*>(object + 0x10);
				uint32_t total, chunkCount;
				std::memcpy(&total, object + 0x08, 4);
				std::memcpy(&chunkCount, object + 0x20, 4);
				if (!list && chunkCount)
					return false;
				for (uint32_t c = 0; c < chunkCount; ++c)
				{
					uint32_t n;
					std::memcpy(&n, list + 16 * c + 8, 4);
					if (n != (c + 1 < chunkCount ? elementsPerChunk : total - c * elementsPerChunk) || !*reinterpret_cast<uint8_t* const*>(list + 16 * c))
						return false;
				}
				chunks = reinterpret_cast<uint8_t* const*>(list);
				per = elementsPerChunk;
				size = elementSize;
				count = total;
				return true;
			}

			uint8_t* At(uint32_t i) const { return chunks[2 * (i / per)] + static_cast<size_t>(i % per) * size; }
		};

		int16_t HalfMetres(float v)
		{
			return static_cast<int16_t>(std::clamp(std::trunc(v * 2.0f), -32768.0f, 32767.0f));
		}

		void RescaleNode(uint8_t* node, int depth)
		{
			if (!node || depth > 8)
				return;
			float minimum[3], maximum[3];
			std::memcpy(minimum, node, 12);
			std::memcpy(maximum, node + 0x10, 12);
			const int16_t box[6] = {HalfMetres(minimum[0]), HalfMetres(maximum[0]), HalfMetres(minimum[1]), HalfMetres(maximum[1]),
			                        HalfMetres(minimum[2]), HalfMetres(maximum[2])};
			std::memcpy(node + 0x20, box, sizeof(box));
			for (int k = 0; k < 4; ++k)
				RescaleNode(*reinterpret_cast<uint8_t**>(node + 0x34 + 8 * k), depth + 1);
		}

		// The navmesh's boxes in half metres instead of quarter metres (§61.8; the code that reads them uses the scaled
		// constants of kNavScaleSites): its polygons' (+0x18: x0 x1 y0 y1 z0 z1) from their vertices, with their centres
		// in that box (+0x28, +0x29: round(x 255)), and its quadtree nodes' (+0x20) from their own minimum and maximum.
		// A vertex is three u16 over the navmesh's box: the quadtree root's minimum, +0x60 its size.
		void RescaleBoxes(uint8_t* m)
		{
			uint8_t* root = *reinterpret_cast<uint8_t**>(m + 0x120);
			Split vertices, indices, polys;
			if (!root || !vertices.Open(*reinterpret_cast<uint8_t**>(m + 0x70), 2730, 6) || !indices.Open(*reinterpret_cast<uint8_t**>(m + 0x80), 8192, 2) ||
			    !polys.Open(*reinterpret_cast<uint8_t**>(m + 0x118), 341, 0x30))
			{
				if (!g_warnedLayout.exchange(true))
					log::Warn("world navmesh grid: a navmesh laid out unlike the game's files kept its boxes (pedestrians may not find their way on it)");
				return;
			}
			float origin[3], size[3];
			std::memcpy(origin, root, 12);
			std::memcpy(size, m + 0x60, 12);
			for (uint32_t p = 0; p < polys.count; ++p)
			{
				uint8_t* poly = polys.At(p);
				uint16_t countBits, first;
				std::memcpy(&countBits, poly + 2, 2);
				std::memcpy(&first, poly + 4, 2);
				const uint32_t n = countBits >> 5;
				if (!n || first + n > indices.count)
					continue;
				float lo[3] = {FLT_MAX, FLT_MAX, FLT_MAX}, hi[3] = {-FLT_MAX, -FLT_MAX, -FLT_MAX}, centre[2] = {0, 0};
				for (uint32_t k = 0; k < n; ++k)
				{
					uint16_t index, q[3];
					std::memcpy(&index, indices.At(first + k), 2);
					if (index >= vertices.count)
						return;
					std::memcpy(q, vertices.At(index), 6);
					for (int a = 0; a < 3; ++a)
					{
						const float v = origin[a] + q[a] / 65535.0f * size[a];
						lo[a] = std::min(lo[a], v);
						hi[a] = std::max(hi[a], v);
						if (a < 2)
							centre[a] += v / n;
					}
				}
				const int16_t box[6] = {HalfMetres(lo[0]), HalfMetres(hi[0]), HalfMetres(lo[1]), HalfMetres(hi[1]), HalfMetres(lo[2]), HalfMetres(hi[2])};
				std::memcpy(poly + 0x18, box, sizeof(box));
				for (int a = 0; a < 2; ++a)
				{
					const float b0 = box[2 * a] * 0.5f, b1 = box[2 * a + 1] * 0.5f;
					poly[0x28 + a] = b1 > b0 ? static_cast<uint8_t>(std::clamp(std::lround((centre[a] - b0) / (b1 - b0) * 255.0f), 0L, 255L)) : 0;
				}
			}
			RescaleNode(root, 0);
		}

		uint32_t Renumber(uint32_t index)
		{
			return index < kOldWidth * kOldWidth ? index / kOldWidth * kNavGridWidth + index % kOldWidth : index;
		}

		// A grid navmesh still numbered for the game's grid (its own index, worked out from where its quadtree's corner
		// lies) gets the wider grid's numbers. A split array: +0x10 the chunks {pointer, count}, +0x20 their count.
		void RenumberNavmesh(uint8_t* m)
		{
			uint32_t own;
			std::memcpy(&own, m + 0x140, 4);
			const uint8_t* root = *reinterpret_cast<uint8_t**>(m + 0x120);
			if (!root || own >= kOldWidth * kOldWidth)
				return;
			float corner[2];
			std::memcpy(corner, root, 8);
			const long x = std::lround((corner[0] + 6000.0f) / 150.0f), y = std::lround((corner[1] + 6000.0f) / 150.0f);
			if (x < 0 || y < 0 || x >= static_cast<long>(kOldWidth) || y >= static_cast<long>(kOldWidth) ||
			    own != static_cast<uint32_t>(y) * kOldWidth + static_cast<uint32_t>(x))
				return; // (numbered for the new grid already, or not a grid square)
			const uint32_t index = Renumber(own);
			std::memcpy(m + 0x140, &index, 4);
			uint32_t adjacent;
			std::memcpy(&adjacent, m + 0x94, 4);
			for (uint32_t k = 0; k < adjacent && k < 32; ++k)
			{
				uint32_t a;
				std::memcpy(&a, m + 0x98 + 4 * k, 4);
				a = Renumber(a);
				std::memcpy(m + 0x98 + 4 * k, &a, 4);
			}
			if (const uint8_t* polys = *reinterpret_cast<uint8_t**>(m + 0x118))
			{
				const uint8_t* chunks = *reinterpret_cast<uint8_t* const*>(polys + 0x10);
				uint32_t chunkCount;
				std::memcpy(&chunkCount, polys + 0x20, 4);
				for (uint32_t c = 0; chunks && c < chunkCount; ++c)
				{
					uint8_t* data = *reinterpret_cast<uint8_t* const*>(chunks + 16 * c);
					uint32_t n;
					std::memcpy(&n, chunks + 16 * c + 8, 4);
					for (uint32_t i = 0; data && i < n; ++i)
					{
						uint16_t navmesh;
						std::memcpy(&navmesh, data + 0x30 * i + 6, 2);
						if (navmesh == own)
						{
							const uint16_t renumbered = static_cast<uint16_t>(index);
							std::memcpy(data + 0x30 * i + 6, &renumbered, 2);
						}
					}
				}
			}
			uint32_t links;
			std::memcpy(&links, m + 0x14C, 4);
			if (uint8_t* link = *reinterpret_cast<uint8_t**>(m + 0x128))
				for (uint32_t i = 0; i < links; ++i, link += 0x1C)
				{
					// two navmesh indices, 14 bits each: where the link starts and where it ends
					uint32_t v;
					std::memcpy(&v, link + 0x18, 4);
					v = (v & 0xF0000000u) | Renumber(v & 0x3FFF) | Renumber(v >> 14 & 0x3FFF) << 14;
					std::memcpy(link + 0x18, &v, 4);
				}
			++g_renumbered;
		}

		void* HookPlace(uint8_t* navmesh, void* resource)
		{
			void* result = g_place(navmesh, resource);
			RenumberNavmesh(navmesh);
			RescaleBoxes(navmesh);
			return result;
		}
	}

	bool Install()
	{
		if (!config::Get().worldNavGrid)
			return false;
		const auto module = pattern::Module::Main();
		for (const NavGridSite& s : kNavGridSites)
			if (std::memcmp(reinterpret_cast<const void*>(module.base + s.rva), s.original, s.length) != 0)
			{
				log::Warn("world navmesh grid: code at {:#x} differs from the build the patches were made for; not patched", s.rva);
				return false;
			}
		for (const NavScaleSite& s : kNavScaleSites)
			if (std::memcmp(reinterpret_cast<const void*>(module.base + s.rva), s.original, s.length) != 0)
			{
				log::Warn("world navmesh grid: code at {:#x} differs from the build the patches were made for; not patched", s.rva);
				return false;
			}
		uint32_t statics[3];
		std::memcpy(statics, reinterpret_cast<const void*>(module.base + kStaticSize), sizeof(statics));
		if (statics[0] != 3 || statics[1] != kOldWidth || statics[2] != kOldWidth ||
		    std::memcmp(reinterpret_cast<const void*>(module.base + kPlace), kPlaceBytes, sizeof(kPlaceBytes)) != 0)
		{
			log::Warn("world navmesh grid: the navmesh code differs from the build the patches were made for; not patched");
			return false;
		}
		// the scaled constants (16-byte aligned: movaps / mulps read them), within 2 GB of the code that reads them
		auto* constants = reinterpret_cast<float(*)[4]>(AllocateNear(module.base, sizeof(kNavScaleConstants)));
		if (!constants)
		{
			log::Warn("world navmesh grid: no memory within 2 GB of the game; not patched");
			return false;
		}
		std::memcpy(constants, kNavScaleConstants, sizeof(kNavScaleConstants));
		DWORD old;
		VirtualProtect(constants, sizeof(kNavScaleConstants), PAGE_READONLY, &old);
		if (MH_CreateHook(reinterpret_cast<void*>(module.base + kPlace), reinterpret_cast<void*>(&HookPlace), reinterpret_cast<void**>(&g_place)) != MH_OK ||
		    MH_EnableHook(reinterpret_cast<void*>(module.base + kPlace)) != MH_OK)
		{
			log::Warn("world navmesh grid: cannot hook the navmesh placement; not patched");
			return false;
		}
		for (const NavScaleSite& s : kNavScaleSites)
		{
			uint8_t bytes[16];
			std::memcpy(bytes, s.original, s.length);
			const int32_t disp = static_cast<int32_t>(reinterpret_cast<intptr_t>(constants[s.constant]) - static_cast<intptr_t>(module.base + s.rva + s.length));
			std::memcpy(bytes + s.dispOffset, &disp, 4);
			Write(module.base + s.rva, bytes, s.length);
		}
		for (const NavGridSite& s : kNavGridSites)
		{
			uint8_t bytes[16];
			std::memcpy(bytes, s.original, s.length);
			if (s.lead[0])
				std::memcpy(bytes, s.lead, s.offset);
			std::memcpy(bytes + s.offset, &s.value, s.size);
			Write(module.base + s.rva, bytes, s.length);
		}
		// the navmesh store's slots: the squares and the dynamic navmeshes (gameconfig's NavMeshes pool says 10100)
		pools::SetMinimum("NavMeshes", kNavGridSpecial);
		const uint32_t size[2] = {kNavGridWidth, kNavGridHeight};
		Write(module.base + kStaticSize + 4, reinterpret_cast<const uint8_t*>(size), sizeof(size));
		log::Info("world navmesh grid: {} x {} squares, dynamic navmeshes from {}, {} code sites patched; boxes in half metres ({} constant reads moved)",
		    kNavGridWidth, kNavGridHeight, kNavGridFirstDynamic, std::size(kNavGridSites), std::size(kNavScaleSites));
		return true;
	}
}
