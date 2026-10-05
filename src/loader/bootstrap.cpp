#include "bootstrap.hpp"

#include <Windows.h>

#include <atomic>
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

		using GetSystemTimeAsFileTimeFn = void(WINAPI*)(LPFILETIME);
		GetSystemTimeAsFileTimeFn g_origGetSystemTimeAsFileTime = nullptr;
		Callback g_onUnpacked = nullptr;

		std::atomic_bool g_unpacked = false;
		std::mutex g_probeMutex;
		ULONGLONG g_lastProbe = 0;
		int g_probeCount = 0;

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
							log::Info("game code decrypted after {} probe(s); base={:#x}", g_probeCount, module.base);
							g_unpacked.store(true, std::memory_order_release);
							g_onUnpacked();
						}
					}
				}
			}
			g_origGetSystemTimeAsFileTime(ft);
		}
	}

	bool Install(Callback onUnpacked)
	{
		g_onUnpacked = onUnpacked;
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
