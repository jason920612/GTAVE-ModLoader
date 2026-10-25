#include "gamememory.hpp"

#include <Windows.h>
#include <dxgi1_6.h>

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
		std::atomic<int64_t> g_cap{-1}; // bytes; 0: none; -1: not worked out yet

		// What the game is told at most: ue5GameMemory MB, or (0) 42% of the card's own memory - on an 8 GB card 3.3 GB,
		// a little over what the game needs for its peds and vehicles there (measured: told 2.6 or 2 GB it still kept
		// 3.1-3.5 and 3.2-3.4 GB, streaming its textures again twice as often; research §60.26).
		int64_t Cap(IDXGIAdapter3* adapter)
		{
			const int setting = config::Get().ue5GameMemory;
			if (setting < 0)
				return 0;
			if (setting > 0)
				return int64_t(setting) << 20;
			DXGI_ADAPTER_DESC1 desc{};
			if (FAILED(adapter->GetDesc1(&desc)) || !desc.DedicatedVideoMemory)
				return 0;
			return int64_t(desc.DedicatedVideoMemory / 100 * 42);
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
					log::Info("ue5 game memory: the game is told {} of the GPU's memory (Windows gives it {} MB now)",
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
			for (IUnknown* object : {static_cast<IUnknown*>(adapter3), static_cast<IUnknown*>(adapter), static_cast<IUnknown*>(factory)})
				if (object)
					object->Release();
			if (!ok)
				log::Warn("ue5 game memory: DXGI could not be hooked; the game keeps the budget Windows gives it");
		}
	}

	void Start()
	{
		if (config::Get().ue5GameMemory < 0)
		{
			log::Info("ue5 game memory: off (ue5GameMemory -1)");
			return;
		}
		std::thread(&Hook).detach();
	}
}
