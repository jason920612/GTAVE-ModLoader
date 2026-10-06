#include "pools.hpp"

#include <Windows.h>

#include <cstdint>
#include <algorithm>
#include <string>
#include <string_view>

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

		void HookCreateStore(uint8_t* store)
		{
			const bool named = *reinterpret_cast<uint16_t*>(store + 0x20) != 0;
			const char* name = named ? *reinterpret_cast<const char**>(store + 0x18) : nullptr;
			if (!store[0x98] && name && std::find(std::begin(kGrown), std::end(kGrown), std::string_view(name)) != std::end(kGrown))
			{
				// The configured size wins over the default, so raise both.
				auto* fallback = reinterpret_cast<uint32_t*>(store + 0x10);
				const uint32_t before = *fallback;
				*fallback = Grow(*fallback);
				if (Node* entry = Find(Joaat(name)))
				{
					log::Info("pools: {} {} -> {}", name, entry->size, Grow(entry->size));
					entry->size = Grow(entry->size);
				}
				else
					log::Info("pools: {} {} -> {}", name, before, *fallback);
			}
			g_original(store);
		}
	}

	bool InstallHooks()
	{
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
		log::Info("pools: hooks installed");
		return true;
	}
}
