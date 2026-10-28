#include "pools.hpp"

#include <Windows.h>
#include <malloc.h>

#include <cstdint>
#include <cstring>
#include <algorithm>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <MinHook.h>

#include "../log.hpp"
#include "../pattern.hpp"

namespace loader::game::pools
{
	namespace
	{
		// Store creation (GTA5_Enhanced.exe +0x15CF40): hashes the store name (+0x18, when +0x20 != 0), looks the hash up
		// in the pool size table and takes that size (otherwise the default at +0x10) for the slot pool (+0x50).
		// Pool size table: +0x08 buckets, +0x10 u16 bucket count; nodes {u32 hash, u32 size, node* next}.
		constexpr const char* kCreateStore =
		    "41 57 41 56 41 54 56 57 55 53 48 83 EC 20 80 B9 98 00 00 00 00 0F 85 ? ? ? ? 48 89 CE 48 8B 3D ? ? ? ? 66 83 79 20 00";
		constexpr size_t kTableOperand = 0x1E + 3; // mov rdi, [rip + table]

		using CreateStoreFn = void (*)(uint8_t* store);
		CreateStoreFn g_original = nullptr;
		uint8_t*** g_table = nullptr;

		struct Node
		{
			uint32_t hash;
			uint32_t size;
			Node* next;
		};

		uint32_t Joaat(const char* text)
		{
			uint32_t h = 0;
			for (; *text; ++text)
			{
				const char c = *text >= 'A' && *text <= 'Z' ? static_cast<char>(*text + 32) : *text;
				h += static_cast<uint8_t>(c);
				h += h << 10;
				h ^= h >> 6;
			}
			h += h << 3;
			h ^= h >> 11;
			return h + (h << 15);
		}

		Node* Find(uint32_t hash)
		{
			uint8_t* table = g_table ? reinterpret_cast<uint8_t*>(*g_table) : nullptr;
			if (!table)
				return nullptr;
			const uint16_t buckets = *reinterpret_cast<uint16_t*>(table + 0x10);
			if (!buckets)
				return nullptr;
			Node** heads = *reinterpret_cast<Node***>(table + 0x08);
			for (Node* n = heads[hash % buckets]; n; n = n->next)
				if (n->hash == hash)
					return n;
			return nullptr;
		}

		// Stores that add-on packs fill (MetaDataStore holds one entry per ped and is full in the base game).
		constexpr std::string_view kGrown[] = {"DwdStore", "DrawableStore", "TxdStore", "FragmentStore", "ClothStore", "MetaDataStore",
		    "ModelBvhDataStore", "ModelBvhDictionaryStore"};

		uint32_t Grow(uint32_t size)
		{
			return size + std::max<uint32_t>(size / 4, 500);
		}

		// Pools and stores the world builder's new land fills (gameconfig's names; at least these sizes): its collision
		// files and maps (a whole island: thousands of tiles), and its entities - trees by the ten thousand where the laser
		// scan saw them. Each tree with collision near the player takes a physics instance: a full "phInstGta" pool
		// (10030 of 0x70 bytes; fwBasePool::New, +0x15A7C0, breaks into int3 when no slot is free) took the game down as
		// the camera came to a palm-lined street (research/phase0.md §61.20). Raising "Dummy Object" or "Building" much
		// further (40000 / 100000) made the game fail at start: their memory comes from a fixed budget. Parked cars: a
		// town's driveways and car parks round the player (research/phase0.md §63), the game's 800 car generators not enough.
		// A land's files take a slot each in the stores while it is there (a big Florida sector 20000 drawables, 7600
		// collision files, 4000 dictionaries, 3000 maps); one put away gives them back (landfiles.cpp, research/phase0.md
		// §63.12-63.14), so the stores hold the game's own and two such lands, with room to spare for files the streaming
		// engine could not unload in time (the streaming objects of all modules about 800000 with these sizes).
		constexpr std::pair<std::string_view, uint32_t> kWorldPools[] = {{"DrawableStore", 161250}, {"TxdStore", 120250}, {"StaticBounds", 56700},
		    {"MapDataStore", 26020}, {"MapTypesStore", 2700}, {"Dummy Object", 24000}, {"Building", 70000}, {"phInstGta", 32000}, {"CCargen", 4000}};

		// Stores that need a size of their own (SetMinimum), by name.
		std::vector<std::pair<std::string, uint32_t>> g_minimum;

		// A store's slot pool (fwBasePool at +0x38) takes its memory from the allocator at the pool's +0 - for every store
		// the game's pool allocator (GTA5_Enhanced.exe +0x286A310: the master allocator's fifth heap, of a fixed size). The
		// stores grown for the lands do not fit in it: with the drawables' store at 261250 slots a pool made as story mode
		// started got no memory (its storage left null, 0x15A514). Those stores' pools come from the process heap instead
		// (the pool calls only +8, to allocate; +0x10 frees).
		struct HeapAllocator
		{
			virtual ~HeapAllocator() = default;
			virtual void* Allocate(size_t size)
			{
				return _aligned_malloc(size, 16);
			}
			virtual void Free(void* p)
			{
				_aligned_free(p);
			}
		};
		HeapAllocator g_heap;

		// The pool size lookup (GTA5_Enhanced.exe +0x149060: table, name hash, default -> the configured size, or the
		// default): every store and pool asks it. A store with a minimum gets at least that.
		constexpr const char* kPoolSize = "44 0F B7 51 10 45 85 D2 74 ? 41 89 D1 48 8B 49 08 89 D0 31 D2 41 F7 F2";
		using PoolSizeFn = uint32_t (*)(void* table, uint32_t hash, uint32_t fallback);
		PoolSizeFn g_poolSize = nullptr;

		uint32_t HookPoolSize(void* table, uint32_t hash, uint32_t fallback)
		{
			uint32_t size = g_poolSize(table, hash, fallback);
			for (const auto& [storeName, minimum] : g_minimum)
				if (size < minimum && hash == Joaat(storeName.c_str()))
				{
					log::Info("pools: {} {} -> {}", storeName, size, minimum);
					size = minimum;
				}
			return size;
		}

		void HookCreateStore(uint8_t* store)
		{
			const bool named = *reinterpret_cast<uint16_t*>(store + 0x20) != 0;
			const char* name = named ? *reinterpret_cast<const char**>(store + 0x18) : nullptr;
			if (!store[0x98] && name && std::find(std::begin(kGrown), std::end(kGrown), std::string_view(name)) != std::end(kGrown))
			{
				// The configured size wins over the default, so raise both - to a minimum set for the store too (these
				// stores take the table's size as it is: the pool size lookup is not asked).
				uint32_t minimum = 0;
				for (const auto& [storeName, size] : g_minimum)
					if (storeName == name)
						minimum = std::max(minimum, size);
				const auto grown = [&](uint32_t size) { return std::max(Grow(size), minimum); };
				auto* fallback = reinterpret_cast<uint32_t*>(store + 0x10);
				const uint32_t before = *fallback;
				*fallback = grown(*fallback);
				if (Node* entry = Find(Joaat(name)))
				{
					log::Info("pools: {} {} -> {}", name, entry->size, grown(entry->size));
					entry->size = grown(entry->size);
				}
				else
					log::Info("pools: {} {} -> {}", name, before, *fallback);
			}
			if (!store[0x98] && name &&
			    (std::find(std::begin(kGrown), std::end(kGrown), std::string_view(name)) != std::end(kGrown) ||
			        std::any_of(std::begin(kWorldPools), std::end(kWorldPools), [&](const auto& p) { return p.first == name; })))
				*reinterpret_cast<void**>(store + 0x38) = &g_heap;
			g_original(store);
		}

		// Vehicle model infos (0x5A0 bytes each) live in blocks of a fixed size, one per key, made by the vehicle
		// archetype factory (vtable slot 4, GTA5_Enhanced.exe +0x136F230: key, count -> {elements, u16 used, u16 size}).
		// Adding a model never checks the size (+0x136F1E0, +0x1377010), so add-on cars past it overwrite memory: the
		// game crashed registering the 641st vehicle of block 0xF000. Large blocks get extra room.
		constexpr const char* kCreateVehicleBlock =
		    "41 57 41 56 56 57 55 53 48 83 EC 38 45 89 C6 49 89 CF 89 54 24 2C 0F B7 49 10 49 83 C7 08 85 C9";
		constexpr size_t kElementSizeOperand = 0x79; // imul rcx, rax, 0x5A0
		using CreateBlockFn = void (*)(void* factory, uint32_t key, int32_t count);
		CreateBlockFn g_createBlock = nullptr;

		void HookCreateVehicleBlock(void* factory, uint32_t key, int32_t count)
		{
			const int32_t grown = count >= 256 ? static_cast<int32_t>(Grow(static_cast<uint32_t>(count))) : count;
			log::Info("pools: vehicle model info block {:#x}: {} -> {}", key, count, grown);
			g_createBlock(factory, key, grown);
		}
	}

	void SetMinimum(const char* name, uint32_t size)
	{
		g_minimum.emplace_back(name, size);
	}

	bool InstallHooks()
	{
		for (const auto& [name, size] : kWorldPools)
			SetMinimum(std::string(name).c_str(), size);
		const auto module = pattern::Module::Main();
		const auto at = pattern::Find(module.text, pattern::Pattern::Parse(kCreateStore));
		if (!at)
		{
			log::Warn("pools: store creation not found; store sizes stay as configured");
			return false;
		}
		g_table = reinterpret_cast<uint8_t***>(pattern::Rip(*at + kTableOperand));
		if (MH_CreateHook(reinterpret_cast<void*>(*at), reinterpret_cast<void*>(&HookCreateStore), reinterpret_cast<void**>(&g_original)) != MH_OK ||
		    MH_EnableHook(reinterpret_cast<void*>(*at)) != MH_OK)
		{
			log::Error("pools: could not hook store creation");
			return false;
		}
		const auto poolSize = pattern::Find(module.text, pattern::Pattern::Parse(kPoolSize));
		if (!poolSize)
			log::Warn("pools: pool size lookup not found; stores keep their configured sizes");
		else if (MH_CreateHook(reinterpret_cast<void*>(*poolSize), reinterpret_cast<void*>(&HookPoolSize), reinterpret_cast<void**>(&g_poolSize)) != MH_OK ||
		         MH_EnableHook(reinterpret_cast<void*>(*poolSize)) != MH_OK)
			log::Error("pools: could not hook the pool size lookup");
		const auto block = pattern::Find(module.text, pattern::Pattern::Parse(kCreateVehicleBlock));
		static constexpr uint8_t kElementSize[] = {0x48, 0x69, 0xC8, 0xA0, 0x05, 0x00, 0x00};
		if (!block || std::memcmp(reinterpret_cast<const void*>(*block + kElementSizeOperand), kElementSize, sizeof(kElementSize)) != 0)
			log::Warn("pools: vehicle model info blocks not found; add-on cars beyond the game's room can crash it");
		else if (MH_CreateHook(reinterpret_cast<void*>(*block), reinterpret_cast<void*>(&HookCreateVehicleBlock),
		             reinterpret_cast<void**>(&g_createBlock)) != MH_OK ||
		         MH_EnableHook(reinterpret_cast<void*>(*block)) != MH_OK)
			log::Error("pools: could not hook the vehicle model info blocks");
		log::Info("pools: hooks installed");
		return true;
	}
}
