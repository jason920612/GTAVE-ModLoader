#include "fixes.hpp"

#include <Windows.h>
#include <d3d12.h>

#include <atomic>
#include <chrono>

#include <MinHook.h>

#include "../debug/hangdump.hpp"
#include "../log.hpp"
#include "../pattern.hpp"

namespace loader::game::fixes
{
	namespace
	{
		// Fence wrapper: +0x10 ID3D12Fence*, +0x18 last completed value seen. Values carry flags above bit 55.
		constexpr uint64_t kValueMask = 0x00FFFFFFFFFFFFFF;
		constexpr int kGiveUpAfterSeconds = 5; // both the wait and the main thread's stall

		using WaitFenceFn = void (*)(void* fence, uint64_t value);
		using LoadingScreenFn = void (*)();
		WaitFenceFn g_origWait = nullptr;
		LoadingScreenFn g_origLoadingScreen = nullptr;
		thread_local bool t_loadingScreen = false;
		std::atomic<int> g_broken = 0;

		void HookLoadingScreen()
		{
			t_loadingScreen = true;
			g_origLoadingScreen();
			t_loadingScreen = false;
		}

		void HookWaitFence(void* wrapper, uint64_t value)
		{
			if (!t_loadingScreen)
				return g_origWait(wrapper, value);
			auto* fence = *reinterpret_cast<ID3D12Fence**>(static_cast<uint8_t*>(wrapper) + 0x10);
			const uint64_t target = value & kValueMask;
			const uint64_t done = fence ? fence->GetCompletedValue() : UINT64_MAX;
			if (!fence || done == UINT64_MAX || done >= target)
				return g_origWait(wrapper, value);

			// Wait the same way as the game, but give up once the main thread has stalled behind us.
			static thread_local HANDLE event = CreateEventW(nullptr, FALSE, FALSE, nullptr);
			if (!event || FAILED(fence->SetEventOnCompletion(target, event)))
				return g_origWait(wrapper, value);
			const auto start = std::chrono::steady_clock::now();
			while (WaitForSingleObject(event, 50) != WAIT_OBJECT_0)
			{
				const auto waited = std::chrono::duration_cast<std::chrono::seconds>(std::chrono::steady_clock::now() - start).count();
				if (waited >= kGiveUpAfterSeconds && debug::hangdump::StalledSeconds() >= kGiveUpAfterSeconds)
				{
					log::Warn("fix: loading screen waited {} s for GPU fence value {} (at {}) while the main thread stalled; "
					          "continuing to break the deadlock (#{})",
					    waited, target, fence->GetCompletedValue(), ++g_broken);
					return;
				}
			}
			g_origWait(wrapper, value); // completed: let the game update its cached value
		}

		bool Hook(uintptr_t at, void* detour, void** original)
		{
			return MH_CreateHook(reinterpret_cast<void*>(at), detour, original) == MH_OK && MH_EnableHook(reinterpret_cast<void*>(at)) == MH_OK;
		}
	}

	bool InstallLoadingScreenDeadlockFix()
	{
		const auto module = pattern::Module::Main();
		// WaitFence(wrapper, value): mov rax,[rcx+18h]; sub rax,rdx; bt rax,37h; jae ...
		const auto wait = pattern::Find(module.text,
		    pattern::Pattern::Parse("41 56 56 57 53 48 83 EC 28 48 8B 41 18 48 29 D0 48 0F BA E0 37 0F 83 ? ? ? ? 48 89 D6 48 89 CF"));
		// Loading screen thread body: ...; xor ecx,ecx; call; call <loading screen update>; nop; add rsp,30h; pop rbx; jmp
		const auto call = pattern::Find(module.text, pattern::Pattern::Parse("E8 ? ? ? ? E8 ? ? ? ? 31 C9 E8 ? ? ? ? E8 ? ? ? ? 90 48 83 C4 30 5B E9"));
		if (!wait || !call)
		{
			log::Warn("fix: loading screen code not found ({} {}); deadlock fix not active", wait.has_value(), call.has_value());
			return false;
		}
		const uintptr_t loadingScreen = pattern::Rip(*call + 0x12);
		if (!Hook(loadingScreen, reinterpret_cast<void*>(&HookLoadingScreen), reinterpret_cast<void**>(&g_origLoadingScreen)) ||
		    !Hook(*wait, reinterpret_cast<void*>(&HookWaitFence), reinterpret_cast<void**>(&g_origWait)))
		{
			log::Warn("fix: could not hook the loading screen; deadlock fix not active");
			return false;
		}
		log::Info("fix: loading screen deadlock fix active (+{:#x}, +{:#x})", loadingScreen - module.base, *wait - module.base);
		return true;
	}
}
