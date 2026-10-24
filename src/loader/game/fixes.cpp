#include "fixes.hpp"

#include <Windows.h>
#include <d3d12.h>

#include <atomic>
#include <chrono>
#include <cstring>

#include <MinHook.h>

#include "../debug/hangdump.hpp"
#include "../log.hpp"
#include "../pattern.hpp"
#include "scripts.hpp"

namespace loader::game::fixes
{
	void KeepOnlineVehicles()
	{
		constexpr uint32_t kContentCheckOff = 4521040;
		if (int64_t* g = scripts::Global(kContentCheckOff); g && *g != 1)
		{
			*g = 1;
			log::Info("fix: Online vehicles allowed in story mode (Global {} set)", kContentCheckOff);
		}
	}

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

		// The flow texel (A8R8G8B8 as the block decoder writes it) to the current: each channel 0..255 -> -1..1, times
		// the scale the legacy game uses.
		void RiverFlow(const uint32_t* texel, float* flow)
		{
			constexpr float kScale = 0.1f;
			flow[0] = (static_cast<float>(*texel >> 16 & 0xFF) / 255.0f * 2.0f - 1.0f) * kScale;
			flow[1] = (static_cast<float>(*texel >> 8 & 0xFF) / 255.0f * 2.0f - 1.0f) * kScale;
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

	bool InstallRiverCurrentFix()
	{
		const auto module = pattern::Module::Main();
		// The flow sampler's tail: mov [rsp+20h],rax; lea rcx,[rsp+28h]; call <BC1 texel>; mov rax,<neutral flow, twice>;
		// mov [r14],rax; jmp <epilogue>
		const auto pattern = pattern::Pattern::Parse("48 89 44 24 20 48 8D 4C 24 28 E8 ? ? ? ? 48 B8 33 9B CD 39 33 9B CD 39 49 89 06 EB ?");
		const auto hit = pattern::Find(module.text, pattern);
		if (!hit || pattern::Count(module.text, pattern) != 1)
		{
			log::Warn("fix: river current sampler not found; rivers stay without currents");
			return false;
		}
		const uintptr_t site = *hit + 15; // the 15 bytes from mov rax to the end of jmp
		const uintptr_t epilogue = *hit + 30 + *reinterpret_cast<const int8_t*>(*hit + 29);

		// Thunk: RiverFlow(&texel, flow out), then on to the epilogue. The sampler's stack is 16-byte aligned here and
		// everything live after this point is in non-volatile registers.
		auto* thunk = static_cast<uint8_t*>(VirtualAlloc(nullptr, 64, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE));
		if (!thunk)
		{
			log::Warn("fix: no memory for the river current thunk");
			return false;
		}
		uint8_t* at = thunk;
		const auto bytes = [&](std::initializer_list<uint8_t> list) {
			for (const uint8_t b : list)
				*at++ = b;
		};
		const auto u64 = [&](uint64_t v) {
			std::memcpy(at, &v, 8);
			at += 8;
		};
		bytes({0x48, 0x8D, 0x4C, 0x24, 0x28}); // lea rcx,[rsp+28h]
		bytes({0x4C, 0x89, 0xF2});             // mov rdx,r14
		bytes({0x48, 0xB8});                   // mov rax,RiverFlow
		u64(reinterpret_cast<uint64_t>(&RiverFlow));
		bytes({0xFF, 0xD0});                         // call rax
		bytes({0xFF, 0x25, 0x00, 0x00, 0x00, 0x00}); // jmp [rip]
		u64(epilogue);
		DWORD old = 0;
		VirtualProtect(thunk, 64, PAGE_EXECUTE_READ, &old);
		FlushInstructionCache(GetCurrentProcess(), thunk, 64);

		uint8_t jump[15] = {0xFF, 0x25, 0x00, 0x00, 0x00, 0x00}; // jmp [rip]; <thunk>; nop
		const uint64_t target = reinterpret_cast<uint64_t>(thunk);
		std::memcpy(jump + 6, &target, 8);
		jump[14] = 0x90;
		if (!VirtualProtect(reinterpret_cast<void*>(site), sizeof(jump), PAGE_EXECUTE_READWRITE, &old))
		{
			log::Warn("fix: cannot write the river current sampler");
			return false;
		}
		std::memcpy(reinterpret_cast<void*>(site), jump, sizeof(jump));
		VirtualProtect(reinterpret_cast<void*>(site), sizeof(jump), old, &old);
		FlushInstructionCache(GetCurrentProcess(), reinterpret_cast<void*>(site), sizeof(jump));
		log::Info("fix: river currents read from the flow maps again (+{:#x})", site - module.base);
		return true;
	}
}
