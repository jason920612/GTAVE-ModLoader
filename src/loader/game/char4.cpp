#include "char4.hpp"

#include <Windows.h>

#include <algorithm>
#include <atomic>
#include <cstring>
#include <filesystem>
#include <format>
#include <fstream>
#include <iterator>
#include <unordered_map>

#include <nlohmann/json.hpp>

#include "../char4/generate.hpp"
#include "../config.hpp"
#include "../convert/resource.hpp"
#include "../log.hpp"
#include "../paths.hpp"
#include "../pattern.hpp"
#include "arrayfault.hpp"
#include "natives.hpp"
#include "scripts.hpp"

namespace loader::game::char4
{
	namespace
	{
		constexpr uint64_t kOfficerNative = 0x1B2366C3F2A5C8DFULL; // GET_BENCHMARK_PASS (public hash; char4::kOfficerNative)
		constexpr uint64_t kPlayerPedId = 0xD80958FC74E988A6ULL;
		constexpr uint64_t kStatGetInt = 0x767FBC2AC802EF3DULL, kStatSetInt = 0xB3271D7AB655B441ULL;

		// Character sheet G2349 (29 slots per entry; its saved copy G143050): field 17 is the bank account (0-2 the
		// protagonists', 4 none). The scripts' account cash cache G62254 has one slot per account.
		constexpr uint32_t kAccount3 = 2349 + 1 + 3 * 29 + 17;
		constexpr uint32_t kSavedAccount3 = 143050 + 1 + 3 * 29 + 17;
		constexpr uint32_t kCashCache = 62254;

		std::filesystem::path Dir()
		{
			return paths::Get().root / L"core" / L"char4";
		}

		std::vector<uint32_t> g_programs; // the rewritten scripts' hashes
		bool g_installed = false;
		std::atomic<int32_t> g_ped = 0;
		std::atomic<bool> g_player = false;
		int64_t g_vanillaAccount[2] = {};
		bool g_accountChanged = false;
		uint32_t g_cashStat = 0; // SP3_TOTAL_CASH

		// ---- the engine's stats slot (research/phase0.md §55.6, "統計槽別名") ----
		// Every per-character stat object holds the hashes {SP0, SP1, SP2, MP0, MP1} of one stat; the engine uses the
		// current stats index (0-2, set from the protagonist models when the player changes) to pick one, and a prefix
		// table (SP0 .. MP1) for names it builds. Slot 4 (MP1, unused in story mode) becomes SP3 while the officer is
		// the player.
		struct Alias
		{
			bool tried = false, found = false, patched = false;
			uint32_t* index = nullptr;
			const char** prefix = nullptr;
			const char* oldPrefix = nullptr;
			std::vector<std::pair<uint32_t*, uint32_t>> slots; // dword 4 of each stat object, and its value
			uint32_t protagonist = 0;                          // the index before the officer's
		} g_alias;

		struct Section
		{
			uint8_t* start;
			size_t size;
		};

		// The readable, committed memory of the game's data sections (not code; protected pages are left out). The game
		// keeps its data pages executable.
		std::vector<Section> DataSections(uintptr_t base)
		{
			std::vector<Section> out;
			const auto* dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(base);
			const auto* nt = reinterpret_cast<const IMAGE_NT_HEADERS64*>(base + dos->e_lfanew);
			const auto* sec = IMAGE_FIRST_SECTION(nt);
			constexpr DWORD kReadable = PAGE_READONLY | PAGE_READWRITE | PAGE_WRITECOPY | PAGE_EXECUTE_READ | PAGE_EXECUTE_READWRITE | PAGE_EXECUTE_WRITECOPY;
			for (WORD i = 0; i < nt->FileHeader.NumberOfSections; ++i)
			{
				if ((sec[i].Characteristics & IMAGE_SCN_MEM_EXECUTE) || !(sec[i].Characteristics & IMAGE_SCN_MEM_READ))
					continue;
				auto* at = reinterpret_cast<uint8_t*>(base + sec[i].VirtualAddress);
				auto* const end = at + sec[i].Misc.VirtualSize;
				while (at < end)
				{
					MEMORY_BASIC_INFORMATION m{};
					if (!VirtualQuery(at, &m, sizeof(m)))
						break;
					auto* const regionEnd = std::min(end, static_cast<uint8_t*>(m.BaseAddress) + m.RegionSize);
					if (m.State == MEM_COMMIT && (m.Protect & kReadable) && !(m.Protect & (PAGE_GUARD | PAGE_NOACCESS)))
						out.push_back({at, static_cast<size_t>(regionEnd - at)});
					at = regionEnd;
				}
			}
			return out;
		}

		bool Inside(const std::vector<Section>& sections, uint64_t p, size_t size)
		{
			for (const Section& s : sections)
				if (p >= reinterpret_cast<uint64_t>(s.start) && p + size <= reinterpret_cast<uint64_t>(s.start) + s.size)
					return true;
			return false;
		}

		// The SP3_ stat names the pack declares (without the prefix).
		std::vector<std::string> StatNames()
		{
			std::ifstream in(Dir() / L"sp3stats.xml", std::ios::binary);
			const std::string text(std::istreambuf_iterator<char>(in), {});
			std::vector<std::string> names;
			for (size_t at = text.find("Name=\"SP3_"); at != std::string::npos; at = text.find("Name=\"SP3_", at + 1))
			{
				const size_t start = at + 10, end = text.find('"', start);
				if (end != std::string::npos)
					names.push_back(text.substr(start, end - start));
			}
			return names;
		}

		bool FindAlias()
		{
			g_alias.tried = true;
			const auto module = pattern::Module::Main();
			// The setter: the player's model against the three protagonists' (0, 1, 2), then the index is stored.
			const auto setter = pattern::Find(module.text, pattern::Pattern::Parse(
			    "74 1E B8 01 00 00 00 3B 0D ? ? ? ? 74 11 B8 02 00 00 00 3B 0D ? ? ? ? 0F 85 ? ? ? ? F6 C3 01 74 1C C6 05 ? ? ? ? 00 89 05 ? ? ? ? 83 F8 03"));
			if (!setter)
			{
				log::Warn("fourth character: the stats index was not found; the officer's money is not shown");
				return false;
			}
			g_alias.index = reinterpret_cast<uint32_t*>(pattern::Rip(*setter + 46));
			const auto sections = DataSections(module.base);
			const auto inModule = [&](uint64_t p) { return p >= module.base && p < module.base + module.size && Inside(sections, p, 4); };
			static constexpr const char* kPrefixes[] = {"SP0", "SP1", "SP2", "MP0", "MP1"};
			for (const Section& s : sections)
			{
				for (size_t off = 0; off + 40 <= s.size && !g_alias.prefix; off += 8)
				{
					const auto* q = reinterpret_cast<const uint64_t*>(s.start + off);
					bool all = true;
					for (int k = 0; k < 5 && all; ++k)
						all = inModule(q[k]) && std::memcmp(reinterpret_cast<const char*>(q[k]), kPrefixes[k], 4) == 0;
					if (all)
						g_alias.prefix = reinterpret_cast<const char**>(s.start + off);
				}
			}
			if (!g_alias.prefix)
			{
				log::Warn("fourth character: the stats prefix table was not found; the officer's money is not shown");
				return false;
			}
			// Stat objects: SP0, SP1, SP2 hashes of a per-character stat in a row.
			struct Hashes
			{
				uint32_t sp1, sp2, sp3;
			};
			std::unordered_map<uint32_t, Hashes> bySp0;
			for (const auto& name : StatNames())
				bySp0[convert::Joaat("SP0_" + name)] = {convert::Joaat("SP1_" + name), convert::Joaat("SP2_" + name), convert::Joaat("SP3_" + name)};
			for (const Section& s : sections)
				for (size_t off = 0; off + 20 <= s.size; off += 4)
				{
					auto* d = reinterpret_cast<uint32_t*>(s.start + off);
					const auto it = bySp0.find(d[0]);
					if (it != bySp0.end() && d[1] == it->second.sp1 && d[2] == it->second.sp2)
						g_alias.slots.push_back({d + 4, it->second.sp3});
				}
			g_alias.found = !g_alias.slots.empty();
			log::Info("fourth character: stats index at rva {:#x}, prefix table at rva {:#x}, {} stat objects", reinterpret_cast<uintptr_t>(g_alias.index) - module.base,
			    reinterpret_cast<uintptr_t>(g_alias.prefix) - module.base, g_alias.slots.size());
			return g_alias.found;
		}

		void Write(void* at, const void* value, size_t size)
		{
			DWORD old = 0;
			if (VirtualProtect(at, size, PAGE_READWRITE, &old))
			{
				std::memcpy(at, value, size);
				VirtualProtect(at, size, old, &old);
			}
		}

		// Slot 4 names the SP3 stats (it stays so; the protagonists never use slot 4 in story mode).
		void PatchAlias()
		{
			static const char kSp3[] = "SP3";
			if (g_alias.patched || (!g_alias.tried && !FindAlias()) || !g_alias.found)
				return;
			g_alias.oldPrefix = g_alias.prefix[4];
			const char* sp3 = kSp3;
			Write(&g_alias.prefix[4], &sp3, sizeof(sp3));
			for (auto& [slot, value] : g_alias.slots)
			{
				const uint32_t old = *slot;
				Write(slot, &value, 4);
				value = old; // kept for UnpatchAlias
			}
			g_alias.patched = true;
		}

		void UnpatchAlias()
		{
			if (!g_alias.patched)
				return;
			if (*g_alias.index == 4)
				Write(g_alias.index, &g_alias.protagonist, 4);
			Write(&g_alias.prefix[4], &g_alias.oldPrefix, sizeof(g_alias.oldPrefix));
			for (auto& [slot, value] : g_alias.slots)
			{
				const uint32_t sp3 = *slot;
				Write(slot, &value, 4);
				value = sp3;
			}
			g_alias.patched = false;
		}

		void SetGlobal(uint32_t index, int64_t value)
		{
			if (int64_t* g = scripts::Global(index); g && *g != value)
				*g = value;
		}

		// The officer is the player: account 3, the stats slot, the cash cache.
		void Hold()
		{
			int64_t* account = scripts::Global(kAccount3);
			int64_t* saved = scripts::Global(kSavedAccount3);
			if (account && saved)
			{
				if (!g_accountChanged)
				{
					g_vanillaAccount[0] = *account;
					g_vanillaAccount[1] = *saved;
					g_accountChanged = true;
				}
				SetGlobal(kAccount3, 3);
				SetGlobal(kSavedAccount3, 3);
			}
			PatchAlias();
			if (g_alias.patched && *g_alias.index != 4)
			{
				if (*g_alias.index < 3)
					g_alias.protagonist = *g_alias.index;
				const uint32_t four = 4;
				Write(g_alias.index, &four, 4);
			}
			auto& cache = arrayfault::Shadows()[kCashCache];
			if (cache.size() < 1)
				cache.resize(1, 0);
			int32_t cash = 0;
			if (natives::Invoke<int32_t>(kStatGetInt, g_cashStat, &cash, -1))
				cache[0] = cash;
		}

		// The officer's unlock stats never hold the forbidden weapons' bits (core\char4\unlockmasks.txt).
		void LimitUnlocks()
		{
			std::ifstream in(Dir() / L"unlockmasks.txt");
			std::string stat;
			uint32_t mask = 0;
			while (in >> stat >> mask)
			{
				const uint32_t hash = convert::Joaat("SP3_" + stat);
				int32_t value = 0;
				if (mask && natives::Invoke<int32_t>(kStatGetInt, hash, &value, -1) && (static_cast<uint32_t>(value) & mask))
				{
					natives::Invoke<int32_t>(kStatSetInt, hash, static_cast<int32_t>(static_cast<uint32_t>(value) & ~mask), 1);
					log::Info("fourth character: forbidden weapons taken out of SP3_{}", stat);
				}
			}
		}

		void Release()
		{
			if (g_accountChanged)
			{
				SetGlobal(kAccount3, g_vanillaAccount[0]);
				SetGlobal(kSavedAccount3, g_vanillaAccount[1]);
				g_accountChanged = false;
			}
			if (g_alias.patched && *g_alias.index == 4)
				Write(g_alias.index, &g_alias.protagonist, 4);
		}

		std::filesystem::path SaveFile(int32_t nonce)
		{
			return Dir() / L"saves" / std::format(L"{:08x}.json", static_cast<uint32_t>(nonce));
		}
	}

	void Prepare(const convert::Decryptor* decrypt)
	{
		std::error_code ec;
		if (!config::Get().fourthCharacter)
		{
			// Off: no rewritten scripts or SP3 stats in the pack (the officer's saved data stays).
			for (const auto& entry : std::filesystem::directory_iterator(Dir(), ec))
				if (entry.is_regular_file())
					std::filesystem::remove(entry.path(), ec);
			log::Info("fourth character: off (loader.json fourthCharacter)");
			return;
		}
		const auto generated = loader::char4::Generate(paths::Get().gameDir, Dir(), decrypt);
		for (const auto& w : generated.warnings)
			log::Warn("fourth character: {}", w);
		if (!generated.ok)
		{
			log::Warn("fourth character: not available (the game's scripts could not be rewritten)");
			return;
		}
		g_programs.clear();
		for (const auto& s : generated.scripts)
			g_programs.push_back(convert::Joaat(s));
		g_cashStat = convert::Joaat("SP3_TOTAL_CASH");
		log::Info("fourth character: {} game script(s) rewritten for it", generated.scripts.size());
	}

	void Install()
	{
		if (g_programs.empty())
			return;
		if (!arrayfault::Serving())
		{
			log::Warn("fourth character: off, the script array hook is not in place");
			return;
		}
		auto* handler = reinterpret_cast<void*>(natives::FindHandler(kOfficerNative));
		if (!handler)
		{
			log::Warn("fourth character: off, the officer native is not known");
			return;
		}
		for (const uint32_t program : g_programs)
			if (!scripts::AddNativeOverride(program, handler, [](void* context, void*) {
				    static_cast<natives::CallContext*>(context)->returnValue[0] = g_player ? 1 : 0;
			    }))
			{
				log::Warn("fourth character: off, no native override slots left");
				return;
			}
		arrayfault::ServeFourth(g_programs);
		g_installed = true;
		log::Info("fourth character: ready");
	}

	void Tick()
	{
		if (!g_installed)
			return;
		const int32_t ped = g_ped;
		const bool player = ped && natives::Invoke<int32_t>(kPlayerPedId) == ped;
		if (player != g_player)
		{
			g_player = player;
			arrayfault::SetFourthActive(player);
			log::Info("fourth character: {} the player", player ? "is" : "is no longer");
			if (!player)
				Release();
		}
		// A moment after the officer becomes the player (the mod sets up a new officer's unlocks then).
		static uint64_t limitAt = 0;
		if (!player)
			limitAt = 0;
		else if (!limitAt)
			limitAt = GetTickCount64() + 1000;
		else if (limitAt != UINT64_MAX && GetTickCount64() >= limitAt)
		{
			limitAt = UINT64_MAX;
			LimitUnlocks();
		}
		if (player)
			Hold();
	}

	void SetPed(int32_t ped)
	{
		g_ped = ped;
	}

	bool Ready()
	{
		return g_installed;
	}

	bool IsPlayer()
	{
		return g_player;
	}

	void Saved(int32_t nonce)
	{
		if (!g_installed || !nonce)
			return;
		nlohmann::json shadows = nlohmann::json::object();
		for (const auto& [global, values] : arrayfault::Shadows())
			if (global != kCashCache)
				shadows[std::to_string(global)] = values;
		std::error_code ec;
		std::filesystem::create_directories(Dir() / L"saves", ec);
		std::ofstream out(SaveFile(nonce), std::ios::trunc);
		out << nlohmann::json{{"version", 1}, {"shadows", shadows}}.dump() << "\n";
		if (!out)
			log::Warn("fourth character: the officer's data could not be written for save {:08x}", static_cast<uint32_t>(nonce));
	}

	void Loading()
	{
		arrayfault::ForgetAddresses();
		// The global blocks are made again from the save: what was changed is gone.
		g_accountChanged = false;
		g_player = false;
		arrayfault::SetFourthActive(false);
	}

	void Loaded(int32_t nonce)
	{
		if (!g_installed)
			return;
		auto& shadows = arrayfault::Shadows();
		shadows.clear();
		arrayfault::ForgetAddresses();
		std::ifstream in(SaveFile(nonce));
		const auto j = in ? nlohmann::json::parse(in, nullptr, false) : nlohmann::json();
		if (j.is_object() && j.contains("shadows") && j["shadows"].is_object())
			for (const auto& [key, values] : j["shadows"].items())
				if (values.is_array())
					shadows[static_cast<uint32_t>(std::stoul(key))] = values.get<std::vector<int64_t>>();
		log::Info("fourth character: {} officer array element(s) for save {:08x}", shadows.size(), static_cast<uint32_t>(nonce));
	}

	void Suspend()
	{
		Release();
		UnpatchAlias();
		g_player = false;
		arrayfault::SetFourthActive(false);
	}
}
