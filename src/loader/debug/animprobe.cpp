#include "animprobe.hpp"

#include <Windows.h>
#include <intrin.h>

#include <atomic>
#include <filesystem>
#include <format>
#include <fstream>
#include <mutex>
#include <set>

#include <MinHook.h>

#include "../log.hpp"
#include "../paths.hpp"
#include "../pattern.hpp"

namespace loader::debug::animprobe
{
	namespace
	{
		// The animation sequence decoder (research/phase0.md §41): (sequence, output frame, frame index, phase, remap table, ...).
		using DecodeFn = void (*)(void*, void*, uint32_t, float, uint64_t, uint64_t, uint64_t, uint64_t);
		DecodeFn g_orig = nullptr;
		uintptr_t g_base = 0;
		std::mutex g_mutex;
		std::set<std::pair<uintptr_t, uintptr_t>> g_seen; // (output, caller)
		std::atomic_int g_logged = 0;

		void Detour(void* seq, void* out, uint32_t frame, float t, uint64_t a5, uint64_t a6, uint64_t a7, uint64_t a8)
		{
			const auto caller = reinterpret_cast<uintptr_t>(_ReturnAddress());
			if (g_logged < 300)
			{
				std::lock_guard lock(g_mutex);
				if (g_seen.insert({reinterpret_cast<uintptr_t>(out), caller}).second)
				{
					++g_logged;
					log::Info("animprobe: seq {} out {} frame {} t {:.3f} remap {:#x} a6 {:#x} a7 {:#x} a8 {:#x} caller {:#x} thread {}", seq, out, frame, t, a5, a6,
					    a7, a8, caller - g_base, GetCurrentThreadId());
				}
			}
			g_orig(seq, out, frame, t, a5, a6, a7, a8);
		}
	}

	void StartIfRequested()
	{
		std::ifstream in(paths::Get().root / L"debug_animprobe.txt");
		std::string rvaText;
		if (!in || !(in >> rvaText))
			return;
		const auto module = pattern::Module::Main();
		g_base = module.base;
		const uintptr_t at = module.base + std::stoull(rvaText, nullptr, 16);
		if (MH_CreateHook(reinterpret_cast<void*>(at), reinterpret_cast<void*>(&Detour), reinterpret_cast<void**>(&g_orig)) == MH_OK &&
		    MH_EnableHook(reinterpret_cast<void*>(at)) == MH_OK)
			log::Info("animprobe: hooked {:#x}", at - module.base);
		else
			log::Error("animprobe: could not hook {}", rvaText);
	}
}
