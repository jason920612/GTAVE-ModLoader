#include "gamememory.hpp"

#include <Windows.h>
#include <dxgi1_6.h>

#include <algorithm>
#include <atomic>
#include <cstdint>
#include <format>
#include <string>
#include <thread>

#include <MinHook.h>

#include "../config.hpp"
#include "../log.hpp"

namespace loader::graphics::gamememory
{
	namespace
	{
		constexpr size_t kQueryVideoMemoryInfo = 14; // (IDXGIAdapter3's vtable)

		using QueryVideoMemoryInfoFn = HRESULT(STDMETHODCALLTYPE*)(IDXGIAdapter3*, UINT, DXGI_MEMORY_SEGMENT_GROUP, DXGI_QUERY_VIDEO_MEMORY_INFO*);
		QueryVideoMemoryInfoFn g_queryVideoMemoryInfo = nullptr;
		IDXGIAdapter3* g_adapter = nullptr; // (the first adapter, kept for the report)
		std::atomic<int64_t> g_cap{-1};     // bytes; 0: none; -1: not worked out yet
		std::atomic<bool> g_ue5 = false, g_started = false;
		std::atomic<int> g_share = -1; // (gameVideoMemory changed while the game runs; -1: the setting's)

		// What the game is told at most:
		// - while UE5 draws the world: ue5GameMemory MB, or (0) 42% of the card's own memory - on an 8 GB card 3.3 GB, a
		//   little over what the game needs for its peds and vehicles there (measured: told 2.6 or 2 GB it still kept
		//   3.1-3.5 and 3.2-3.4 GB, streaming its textures again twice as often; research §60.26);
		// - always: gameVideoMemory percent of the card's own memory (0: what Windows gives it).
		// The smaller of the two.
		int64_t Cap(IDXGIAdapter3* adapter)
		{
			DXGI_ADAPTER_DESC1 desc{};
			const int64_t card = SUCCEEDED(adapter->GetDesc1(&desc)) ? int64_t(desc.DedicatedVideoMemory) : 0;
			int64_t cap = 0;
			if (g_ue5)
			{
				const int setting = config::Get().ue5GameMemory;
				if (setting > 0)
					cap = int64_t(setting) << 20;
				else if (setting == 0 && card)
					cap = card / 100 * 42;
			}
			if (const int share = g_share >= 0 ? g_share.load() : config::Get().gameVideoMemory; share > 0 && share < 100 && card)
				cap = cap ? std::min(cap, card / 100 * share) : card / 100 * share;
			return cap;
		}

		HRESULT STDMETHODCALLTYPE QueryVideoMemoryInfo(IDXGIAdapter3* self, UINT node, DXGI_MEMORY_SEGMENT_GROUP group, DXGI_QUERY_VIDEO_MEMORY_INFO* info)
		{
			const HRESULT hr = g_queryVideoMemoryInfo(self, node, group, info);
			if (FAILED(hr) || !info || group != DXGI_MEMORY_SEGMENT_GROUP_LOCAL)
				return hr;
			int64_t cap = g_cap.load(std::memory_order_relaxed);
			if (cap < 0)
			{
				cap = Cap(self);
				if (g_cap.exchange(cap) < 0)
					log::Info("game memory: the game is told {} of the GPU's memory (Windows gives it {} MB now)",
					    cap ? std::format("at most {} MB", cap >> 20) : std::string("what Windows gives it"), info->Budget >> 20);
			}
			if (cap > 0 && info->Budget > uint64_t(cap))
				info->Budget = uint64_t(cap);
			return hr;
		}

		void Hook()
		{
			IDXGIFactory1* factory = nullptr;
			IDXGIAdapter1* adapter = nullptr;
			IDXGIAdapter3* adapter3 = nullptr;
			bool ok = false;
			if (SUCCEEDED(CreateDXGIFactory1(IID_PPV_ARGS(&factory))) && SUCCEEDED(factory->EnumAdapters1(0, &adapter)) &&
			    SUCCEEDED(adapter->QueryInterface(IID_PPV_ARGS(&adapter3))))
			{
				void* target = (*reinterpret_cast<void***>(adapter3))[kQueryVideoMemoryInfo];
				ok = MH_CreateHook(target, reinterpret_cast<void*>(&QueryVideoMemoryInfo), reinterpret_cast<void**>(&g_queryVideoMemoryInfo)) == MH_OK &&
				     MH_EnableHook(target) == MH_OK;
			}
			if (ok)
				g_adapter = adapter3, adapter3 = nullptr;
			for (IUnknown* object : {static_cast<IUnknown*>(adapter3), static_cast<IUnknown*>(adapter), static_cast<IUnknown*>(factory)})
				if (object)
					object->Release();
			if (!ok)
				log::Warn("game memory: DXGI could not be hooked; the game keeps the budget Windows gives it");
		}
	}

	void Start()
	{
		if (g_started.exchange(true))
			return;
		std::thread(&Hook).detach();
	}

	void SetShare(int percent)
	{
		g_share = percent;
		g_cap = -1;
	}

	void SetUe5(bool on)
	{
		g_ue5 = on;
		g_cap = -1; // (worked out again the next time the game asks)
	}

	std::string Report()
	{
		if (!g_adapter || !g_queryVideoMemoryInfo)
			return "err DXGI not hooked";
		DXGI_QUERY_VIDEO_MEMORY_INFO local{}, system{};
		if (FAILED(g_queryVideoMemoryInfo(g_adapter, 0, DXGI_MEMORY_SEGMENT_GROUP_LOCAL, &local)) ||
		    FAILED(g_queryVideoMemoryInfo(g_adapter, 0, DXGI_MEMORY_SEGMENT_GROUP_NON_LOCAL, &system)))
			return "err QueryVideoMemoryInfo failed";
		DXGI_ADAPTER_DESC1 desc{};
		g_adapter->GetDesc1(&desc);
		const int64_t cap = g_cap.load();
		return std::format("ok card {} MB | windows gives {} MB | the game is told {} | the game uses {} MB of the card's, {} MB of the system's "
		                   "(budget there {} MB)",
		    desc.DedicatedVideoMemory >> 20, local.Budget >> 20, cap > 0 ? std::format("{} MB", cap >> 20) : std::string("that"),
		    local.CurrentUsage >> 20, system.CurrentUsage >> 20, system.Budget >> 20);
	}
}
