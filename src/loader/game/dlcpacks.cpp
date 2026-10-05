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
		constexpr uint32_t kEncryptionNg = 0x0FEFFFFF;    // what the game's own archives use

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
		std::unordered_map<std::string, CacheEntry*> g_entries; // normalized archive path as the game opens it

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

		// Reads the table of contents of the archive at `offset`. Only unencrypted archives get
		// an entry (encrypted ones are opened by the game as usual).
		CacheEntry* LoadToc(std::ifstream& in, uint64_t offset, std::string& error)
		{
			uint32_t header[4] = {};
			in.clear();
			in.seekg(static_cast<std::streamoff>(offset));
			if (!in.read(reinterpret_cast<char*>(header), sizeof(header)) || header[0] != kMagic)
			{
				error = "not an RPF7 archive";
				return nullptr;
			}
			if (header[3] != kEncryptionOpen)
			{
				error = "encrypted, used as it is";
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
			reinterpret_cast<uint32_t*>(toc)[3] = kEncryptionNg;
			auto* entry = static_cast<CacheEntry*>(HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, sizeof(CacheEntry)));
			entry->toc = toc;
			entry->plain = 1;
			return entry;
		}

		// A binary file inside an archive (entry layout: research/phase0.md §11).
		struct TocFile
		{
			std::string path; // inside the archive, '/' separated
			uint64_t offset;  // from the start of the archive
			uint32_t packed;  // deflated size, 0 = stored
			uint32_t size;
		};

		std::vector<TocFile> ListFiles(const CacheEntry& archive)
		{
			const auto* header = reinterpret_cast<const uint32_t*>(archive.toc);
			const uint32_t count = header[1], namesSize = header[2] & 0x0FFFFFFF, shift = (header[2] >> 28) & 7;
			const uint8_t* entries = archive.toc + 16;
			const char* names = reinterpret_cast<const char*>(entries + static_cast<size_t>(count) * 16);
			const auto name = [&](uint32_t offset) -> std::string {
				offset <<= shift;
				return offset < namesSize ? std::string(names + offset, strnlen(names + offset, namesSize - offset)) : std::string();
			};
			std::vector<TocFile> files;
			const auto walk = [&](auto&& self, uint32_t index, const std::string& prefix, int depth) -> void {
				if (index >= count || depth > 32)
					return;
				const uint8_t* e = entries + static_cast<size_t>(index) * 16;
				uint64_t q;
				uint32_t a, b;
				std::memcpy(&q, e, 8);
				std::memcpy(&a, e + 8, 4);
				std::memcpy(&b, e + 12, 4);
				if (static_cast<uint32_t>(q >> 32) == 0x7FFFFF00) // directory: first child, child count
				{
					const std::string dir = index == 0 ? std::string() : prefix + name(static_cast<uint32_t>(q)) + "/";
					for (uint32_t i = a; i < a + b && i < count; ++i)
						self(self, i, dir, depth + 1);
				}
				else if (!(q >> 63)) // binary file (resources are never archives or setup files)
					files.push_back({prefix + name(static_cast<uint32_t>(q & 0xFFFF)), ((q >> 40) & 0x7FFFFF) * 512,
					    static_cast<uint32_t>((q >> 16) & 0xFFFFFF), a});
			};
			walk(walk, 0, std::string(), 0);
			return files;
		}

		// Raw deflate, as used inside RPF archives, through the zlib1.dll that ships with the game.
		bool Inflate(const std::vector<uint8_t>& packed, std::vector<uint8_t>& out)
		{
			struct ZStream
			{
				const uint8_t* next_in;
				uint32_t avail_in;
				uint32_t total_in;
				uint8_t* next_out;
				uint32_t avail_out;
				uint32_t total_out;
				const char* msg;
				void* state;
				void* zalloc;
				void* zfree;
				void* opaque;
				int data_type;
				uint32_t adler;
				uint32_t reserved;
			};
			using InitFn = int(__cdecl*)(ZStream*, int, const char*, int);
			using InflateFn = int(__cdecl*)(ZStream*, int);
			using EndFn = int(__cdecl*)(ZStream*);
			static const HMODULE zlib = LoadLibraryW(L"zlib1.dll");
			const auto init = zlib ? reinterpret_cast<InitFn>(GetProcAddress(zlib, "inflateInit2_")) : nullptr;
			const auto inflate = zlib ? reinterpret_cast<InflateFn>(GetProcAddress(zlib, "inflate")) : nullptr;
			const auto end = zlib ? reinterpret_cast<EndFn>(GetProcAddress(zlib, "inflateEnd")) : nullptr;
			if (!init || !inflate || !end)
				return false;
			ZStream z{};
			z.next_in = packed.data();
			z.avail_in = static_cast<uint32_t>(packed.size());
			z.next_out = out.data();
			z.avail_out = static_cast<uint32_t>(out.size());
			if (init(&z, -15, "1.2.11", static_cast<int>(sizeof(ZStream))) != 0)
				return false;
			const int status = inflate(&z, 4 /* Z_FINISH */);
			end(&z);
			return status == 1 /* Z_STREAM_END */ && z.total_out == out.size();
		}

		std::string ReadText(std::ifstream& in, const TocFile& file)
		{
			std::vector<uint8_t> data(file.packed ? file.packed : file.size);
			in.clear();
			in.seekg(static_cast<std::streamoff>(file.offset));
			if (data.empty() || !in.read(reinterpret_cast<char*>(data.data()), static_cast<std::streamsize>(data.size())))
				return {};
			if (file.packed)
			{
				std::vector<uint8_t> plain(file.size);
				if (!Inflate(data, plain))
					return {};
				data = std::move(plain);
			}
			return std::string(data.begin(), data.end());
		}

		std::string XmlValue(const std::string& xml, std::string_view tag)
		{
			const std::string open = "<" + std::string(tag) + ">", close = "</" + std::string(tag) + ">";
			const size_t a = xml.find(open);
			const size_t b = a == xml.npos ? a : xml.find(close, a);
			return b == xml.npos ? std::string() : xml.substr(a + open.size(), b - a - open.size());
		}

		// Prepares an unencrypted pack: its dlc.rpf, and the archives inside it, which the game
		// opens as "<deviceName>:/<path>" (deviceName from the pack's setup2.xml).
		void IndexPack(const Pack& pack)
		{
			std::ifstream in(pack.dir / L"dlc.rpf", std::ios::binary);
			std::string error;
			CacheEntry* root = LoadToc(in, 0, error);
			if (!root)
			{
				log::Info("dlc pack {}: {}", pack.name, error);
				return;
			}
			std::unordered_map<std::string, CacheEntry*> found{{Normalize(pack.path + "dlc.rpf"), root}};
			const auto files = ListFiles(*root);
			const auto setup = std::find_if(files.begin(), files.end(), [](const TocFile& f) { return Normalize(f.path) == "setup2.xml"; });
			const std::string device = setup == files.end() ? std::string() : XmlValue(ReadText(in, *setup), "deviceName");
			if (device.empty())
				log::Warn("dlc pack {}: no deviceName in setup2.xml; archives inside it will not load", pack.name);
			else
				for (const TocFile& file : files)
				{
					const std::string path = Normalize(file.path);
					if (path.size() < 4 || path.compare(path.size() - 4, 4, ".rpf") != 0)
						continue;
					if (file.packed)
					{
						log::Warn("dlc pack {}: {} is compressed and cannot be opened", pack.name, file.path);
						continue;
					}
					std::string nestedError;
					if (CacheEntry* nested = LoadToc(in, file.offset, nestedError))
						found[Normalize(device) + ":/" + path] = nested;
				}
			log::Info("dlc pack {}: {} unencrypted archive(s), device {}", pack.name, found.size(), device);
			std::lock_guard lock(g_mutex);
			g_entries.merge(found);
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
				bool known;
				{
					std::lock_guard lock(g_mutex);
					known = g_entries.contains(Normalize(pack.path + "dlc.rpf"));
				}
				if (!known)
					IndexPack(pack);

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
