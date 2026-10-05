#include "dlcpacks.hpp"

#include <Windows.h>

#include <algorithm>
#include <cstring>
#include <fstream>
#include <mutex>
#include <unordered_map>

#include <MinHook.h>

#include "../config.hpp"
#include "../log.hpp"
#include "../paths.hpp"
#include "../pattern.hpp"

namespace loader::game::dlcpacks
{
	namespace
	{
		// RPF7 header: magic, entry count, names length (bits 28-30: name shift), encryption.
		constexpr uint32_t kMagic = 0x52504637;           // '7FPR'
		constexpr uint32_t kEncryptionOpen = 0x4E45504F;  // 'OPEN': unencrypted, written by tools/rpf_pack.py
		constexpr uint32_t kEncryptionAes = 0x0FFFFFF9;   // a value the game accepts everywhere

		// Registers one DLC folder ("<path>dlc.rpf"); returns whether it was added.
		using RegisterFn = bool (*)(void* manager, const char* path);
		// Processes the DLC list after dlclist.xml was read (also scans platform:/dlcPacks/).
		using ProcessFn = void (*)(void* manager);
		// rpf.cache lookup used when an archive is opened. A hit carries the archive's table of
		// contents; when its "decrypted" flag is set the game uses it as is and never decrypts.
		// The game's decryption code rejects unencrypted archives (int3) and cannot be hooked
		// (it is restored by the executable's protection), so our archives come through here.
		using CacheLookupFn = void* (*)(void* device, const char* path);

		struct CacheEntry
		{
			uint8_t* toc;   // header + entries + names, plain
			uint8_t used;   // +0x08, set by the game
			uint8_t plain;  // +0x09, 1 = already decrypted
			uint8_t pad[0x40 - 10];
		};

		RegisterFn g_register = nullptr;
		ProcessFn g_origProcess = nullptr;
		CacheLookupFn g_origLookup = nullptr;

		std::mutex g_mutex;
		std::vector<Pack> g_packs;
		std::unordered_map<std::string, CacheEntry*> g_entries; // normalized "<path>dlc.rpf"

		std::string Utf8(const std::u8string& s)
		{
			return std::string(s.begin(), s.end());
		}

		std::string Normalize(std::string_view path)
		{
			std::string s(path);
			for (char& c : s)
				c = c == '\\' ? '/' : (c >= 'A' && c <= 'Z' ? static_cast<char>(c - 'A' + 'a') : c);
			return s;
		}

		// Reads the table of contents of an unencrypted archive; null if it is not one.
		CacheEntry* LoadToc(const std::filesystem::path& file, std::string& error)
		{
			std::ifstream in(file, std::ios::binary);
			uint32_t header[4] = {};
			if (!in.read(reinterpret_cast<char*>(header), sizeof(header)) || header[0] != kMagic)
			{
				error = "not an RPF7 archive";
				return nullptr;
			}
			if (header[3] != kEncryptionOpen)
			{
				error = "encrypted archives are used as they are";
				return nullptr;
			}
			const size_t size = 16 + static_cast<size_t>(header[1]) * 16 + (header[2] & 0x0FFFFFFF);
			if (header[1] == 0 || size > 64u << 20)
			{
				error = "corrupt header";
				return nullptr;
			}
			// Never freed: the game keeps using the table for the whole session.
			auto* toc = static_cast<uint8_t*>(HeapAlloc(GetProcessHeap(), 0, size));
			std::memcpy(toc, header, 16);
			if (!in.read(reinterpret_cast<char*>(toc + 16), static_cast<std::streamsize>(size - 16)))
			{
				HeapFree(GetProcessHeap(), 0, toc);
				error = "truncated table of contents";
				return nullptr;
			}
			reinterpret_cast<uint32_t*>(toc)[3] = kEncryptionAes;
			auto* entry = static_cast<CacheEntry*>(HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, sizeof(CacheEntry)));
			entry->toc = toc;
			entry->plain = 1;
			return entry;
		}

		std::vector<Pack> Discover()
		{
			std::vector<Pack> packs;
			const auto& disabled = config::Get().disabledAssets;
			std::error_code ec;
			for (const auto& entry : std::filesystem::directory_iterator(paths::Get().mods, ec))
			{
				if (!entry.is_directory() || !std::filesystem::is_regular_file(entry.path() / L"dlc.rpf", ec))
					continue;
				Pack pack;
				pack.name = Utf8(entry.path().filename().u8string());
				// RAGE paths are UTF-8, use forward slashes and end with a separator.
				pack.path = Utf8(entry.path().generic_u8string()) + "/";
				pack.dir = entry.path();
				pack.enabled = !disabled.contains(pack.name);
				packs.push_back(std::move(pack));
			}
			std::sort(packs.begin(), packs.end(), [](const Pack& a, const Pack& b) { return a.name < b.name; });
			return packs;
		}

		void* HookLookup(void* device, const char* path)
		{
			if (path)
			{
				std::lock_guard lock(g_mutex);
				if (const auto it = g_entries.find(Normalize(path)); it != g_entries.end())
					return it->second;
			}
			return g_origLookup(device, path);
		}

		void HookProcess(void* manager)
		{
			auto packs = Discover();
			for (Pack& pack : packs)
			{
				if (!pack.enabled)
				{
					log::Info("dlc pack {}: disabled", pack.name);
					continue;
				}
				// The game processes the list again when a session starts: reuse what we read.
				const std::string key = Normalize(pack.path + "dlc.rpf");
				bool known;
				{
					std::lock_guard lock(g_mutex);
					known = g_entries.contains(key);
				}
				if (!known)
				{
					std::string error;
					if (CacheEntry* entry = LoadToc(pack.dir / L"dlc.rpf", error))
					{
						std::lock_guard lock(g_mutex);
						g_entries[key] = entry;
					}
					else
						log::Info("dlc pack {}: {}", pack.name, error);
				}

				pack.registered = g_register(manager, pack.path.c_str());
				if (pack.registered)
					log::Info("dlc pack {}: added", pack.name);
				else
					log::Error("dlc pack {}: the game rejected {}dlc.rpf", pack.name, pack.path);
			}
			{
				std::lock_guard lock(g_mutex);
				g_packs = std::move(packs);
			}
			g_origProcess(manager);
		}

		bool Hook(uintptr_t at, void* detour, void** original)
		{
			return MH_CreateHook(reinterpret_cast<void*>(at), detour, original) == MH_OK && MH_EnableHook(reinterpret_cast<void*>(at)) == MH_OK;
		}
	}

	bool InstallHooks()
	{
		const auto module = pattern::Module::Main();
		const auto find = [&](const char* sig) { return pattern::Find(module.text, pattern::Pattern::Parse(sig)); };
		const auto reg = find("41 57 41 56 56 57 53 48 81 EC 40 02 00 00 48 89 D6 49 89 CE 0F 57 C0");
		const auto process = find("41 57 41 56 41 55 41 54 56 57 55 53 48 81 EC 48 01 00 00 49 89 CF 48 8D 0D ? ? ? ? B2 01 E8");
		const auto lookup = find("41 57 41 56 41 55 41 54 56 57 53 48 81 EC 20 02 00 00 49 89 D6 48 89 CF 48 8B 01 FF 90 78 01 00 00");
		if (!reg || !process || !lookup)
		{
			log::Error("dlc packs: game code not found ({} {} {}); packs disabled", reg.has_value(), process.has_value(), lookup.has_value());
			return false;
		}
		g_register = reinterpret_cast<RegisterFn>(*reg);
		if (!Hook(*lookup, reinterpret_cast<void*>(&HookLookup), reinterpret_cast<void**>(&g_origLookup)) ||
		    !Hook(*process, reinterpret_cast<void*>(&HookProcess), reinterpret_cast<void**>(&g_origProcess)))
		{
			log::Error("dlc packs: could not hook the game");
			return false;
		}
		log::Info("dlc packs: hooks installed");
		return true;
	}

	std::vector<Pack> Snapshot()
	{
		std::lock_guard lock(g_mutex);
		return g_packs;
	}
}
