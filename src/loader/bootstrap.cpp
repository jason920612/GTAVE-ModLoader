#include "bootstrap.hpp"

#include <Windows.h>

#include <atomic>
#include <chrono>
#include <mutex>

#include <MinHook.h>

#include "log.hpp"
#include "pattern.hpp"

namespace loader::bootstrap
{
	namespace
	{
		// Present only in decrypted code: the call to the initial file-system mount.
		constexpr auto kUnpackedProbe = "e8 ? ? ? ? 48 8d 05 ? ? ? ? 66 c7 44 58 02 00 00 66 89 3d";

		// Phase 0 signatures; resolved here as a self-test until the subsystems that use them land.
		struct Signature
		{
			const char* name;
			const char* pattern;
		};
		constexpr Signature kSignatures[] = {
			{"InitNativeTables", "EB 2A 0F 1F 40 00 48 8B 54 17 10"},
			{"RunScriptThreads", "BE 40 5D C6 00"},
			{"ScriptThreads", "48 8B 05 ? ? ? ? 48 89 34 F8 48 FF C7 48 39 FB 75 97"},
			{"ScriptPrograms", "48 C7 84 C8 D8 00 00 00 00 00 00 00"},
			{"RpfCacheCheck", "e8 ? ? ? ? 48 8d 0d ? ? ? ? 48 8d 15 ? ? ? ? 48 8d 35"},
			{"DeviceOpenBulk", "41 56 56 57 53 48 81 ec ? ? ? ? 4c 89 c7 66 c7 ? ? ? ? ? 4c 8d 74 24"},
			{"DeviceGetAttributes", "41 56 56 57 53 48 81 ec ? ? ? ? 66 c7 ? ? ? ? ? 4c 8d 74 24"},
			{"DeviceOpenBulkDS", "41 57 41 56 56 57 53 48 81 ec 60 02 00 00 48 89 d3 48 89 cf"},
			{"PackfileFindEncryption", "e8 ? ? ? ? 80 7c 24 2e ? 74 ? 48 8b 56"},
			{"PackfileDecryptHeader", "e8 ? ? ? ? 8b 46 ? 85 c0 74 ? b9"},
			{"PackfileParseHeader", "c6 86 ? ? ? ? ? 48 89 f1 4c 89 fa 45 89 f0 45 31 c9"},
			{"InitialMount", kUnpackedProbe},
			{"OpenArchive", "41 57 41 56 41 55 41 54 56 57 55 53 48 81 ec ? ? ? ? 45 89 cd 45 89 c6 49 89 d7 48 89 ce 8b 05"},
			{"GetDevice", "41 57 41 56 41 55 41 54 56 57 55 53 48 81 ec ? ? ? ? 89 d6 49 89 ce"},
		};

		using GetSystemTimeAsFileTimeFn = void(WINAPI*)(LPFILETIME);
		GetSystemTimeAsFileTimeFn g_origGetSystemTimeAsFileTime = nullptr;

		std::atomic_bool g_unpacked = false;
		std::mutex g_probeMutex;
		ULONGLONG g_lastProbe = 0;
		int g_probeCount = 0;

		void OnGameUnpacked()
		{
			const auto module = pattern::Module::Main();
			log::Info("game code decrypted after {} probe(s); base={:#x}", g_probeCount, module.base);

			const auto start = std::chrono::steady_clock::now();
			int missing = 0;
			for (const auto& sig : kSignatures)
			{
				const auto pat = pattern::Pattern::Parse(sig.pattern);
				const auto hit = pattern::Find(module.text, pat);
				const auto count = hit ? pattern::Count(module.text, pat) : 0;
				if (!hit)
				{
					++missing;
					log::Error("signature {:<24} NOT FOUND", sig.name);
				}
				else
				{
					log::Info("signature {:<24} rva={:#x}{}", sig.name, *hit - module.base, count > 1 ? " (AMBIGUOUS)" : "");
				}
			}
			const auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - start).count();
			log::Info("signature self-test: {}/{} found in {}ms", std::size(kSignatures) - missing, std::size(kSignatures), ms);
		}

		void WINAPI HookGetSystemTimeAsFileTime(LPFILETIME ft)
		{
			if (!g_unpacked.load(std::memory_order_acquire))
			{
				// Probe at most every 10ms; other threads just pass through while one probes.
				std::unique_lock lock(g_probeMutex, std::try_to_lock);
				if (lock.owns_lock() && !g_unpacked)
				{
					const auto now = GetTickCount64();
					if (now - g_lastProbe >= 10)
					{
						g_lastProbe = now;
						++g_probeCount;
						const auto module = pattern::Module::Main();
						if (pattern::Find(module.text, pattern::Pattern::Parse(kUnpackedProbe)))
						{
							OnGameUnpacked();
							g_unpacked.store(true, std::memory_order_release);
						}
					}
				}
			}
			g_origGetSystemTimeAsFileTime(ft);
		}
	}

	bool Install()
	{
		if (MH_Initialize() != MH_OK)
		{
			log::Error("MH_Initialize failed");
			return false;
		}

		void* target = nullptr;
		const auto status = MH_CreateHookApiEx(L"kernel32", "GetSystemTimeAsFileTime", reinterpret_cast<void*>(&HookGetSystemTimeAsFileTime),
		    reinterpret_cast<void**>(&g_origGetSystemTimeAsFileTime), &target);
		if (status != MH_OK || MH_EnableHook(target) != MH_OK)
		{
			log::Error("could not hook GetSystemTimeAsFileTime: {}", MH_StatusToString(status));
			return false;
		}
		log::Info("bootstrap hook installed, waiting for game code to decrypt");
		return true;
	}
}
