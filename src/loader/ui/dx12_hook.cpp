#include "dx12_hook.hpp"

#include <Windows.h>

#include <atomic>

#include "../log.hpp"

namespace loader::ui::dx12
{
	namespace
	{
		// IDXGISwapChain / IDXGISwapChain1 / IDXGISwapChain3 vtable slots.
		constexpr size_t kPresent = 8;
		constexpr size_t kResizeBuffers = 13;
		constexpr size_t kPresent1 = 22;
		constexpr size_t kResizeBuffers1 = 39;

		using PresentFn = HRESULT(STDMETHODCALLTYPE*)(IDXGISwapChain3*, UINT, UINT);
		using Present1Fn = HRESULT(STDMETHODCALLTYPE*)(IDXGISwapChain3*, UINT, UINT, const DXGI_PRESENT_PARAMETERS*);
		using ResizeBuffersFn = HRESULT(STDMETHODCALLTYPE*)(IDXGISwapChain3*, UINT, UINT, UINT, DXGI_FORMAT, UINT);
		using ResizeBuffers1Fn = HRESULT(STDMETHODCALLTYPE*)(IDXGISwapChain3*, UINT, UINT, UINT, DXGI_FORMAT, UINT, const UINT*, IUnknown* const*);

		Callbacks g_callbacks{};
		size_t g_queueOffset = 0;
		PresentFn g_origPresent = nullptr;
		Present1Fn g_origPresent1 = nullptr;
		ResizeBuffersFn g_origResizeBuffers = nullptr;
		ResizeBuffers1Fn g_origResizeBuffers1 = nullptr;
		thread_local bool t_inHook = false;

		ID3D12CommandQueue* QueueOf(IDXGISwapChain3* swapChain)
		{
			return *reinterpret_cast<ID3D12CommandQueue**>(reinterpret_cast<uint8_t*>(swapChain) + g_queueOffset);
		}

		void BeforePresent(IDXGISwapChain3* swapChain)
		{
			if (t_inHook)
				return;
			t_inHook = true;
			g_callbacks.onPresent(swapChain, QueueOf(swapChain));
			t_inHook = false;
		}

		HRESULT STDMETHODCALLTYPE HookPresent(IDXGISwapChain3* swapChain, UINT sync, UINT flags)
		{
			if (!(flags & DXGI_PRESENT_TEST))
				BeforePresent(swapChain);
			return g_origPresent(swapChain, sync, flags);
		}

		HRESULT STDMETHODCALLTYPE HookPresent1(IDXGISwapChain3* swapChain, UINT sync, UINT flags, const DXGI_PRESENT_PARAMETERS* params)
		{
			if (!(flags & DXGI_PRESENT_TEST))
				BeforePresent(swapChain);
			return g_origPresent1(swapChain, sync, flags, params);
		}

		HRESULT STDMETHODCALLTYPE HookResizeBuffers(IDXGISwapChain3* swapChain, UINT count, UINT w, UINT h, DXGI_FORMAT fmt, UINT flags)
		{
			g_callbacks.onReleaseBuffers(swapChain);
			return g_origResizeBuffers(swapChain, count, w, h, fmt, flags);
		}

		HRESULT STDMETHODCALLTYPE HookResizeBuffers1(IDXGISwapChain3* swapChain, UINT count, UINT w, UINT h, DXGI_FORMAT fmt, UINT flags,
		    const UINT* nodeMasks, IUnknown* const* queues)
		{
			g_callbacks.onReleaseBuffers(swapChain);
			return g_origResizeBuffers1(swapChain, count, w, h, fmt, flags, nodeMasks, queues);
		}

		template<class Fn>
		bool PatchSlot(void** vtable, size_t index, void* hook, Fn& original)
		{
			DWORD old = 0;
			if (!VirtualProtect(&vtable[index], sizeof(void*), PAGE_READWRITE, &old))
				return false;
			original = reinterpret_cast<Fn>(vtable[index]);
			InterlockedExchangePointer(&vtable[index], hook);
			VirtualProtect(&vtable[index], sizeof(void*), old, &old);
			return true;
		}

		template<class T>
		void SafeRelease(T*& p)
		{
			if (p)
				p->Release();
			p = nullptr;
		}

		constexpr size_t kScanBytes = 0x1000;

		bool SafeReadPointer(const void* at, void*& out)
		{
			__try
			{
				out = *static_cast<void* const*>(at);
				return true;
			}
			__except (EXCEPTION_EXECUTE_HANDLER)
			{
				return false;
			}
		}

		// Offset of `needle` within the first kScanBytes of `object`, or 0.
		size_t FindPointer(void* object, void* needle)
		{
			for (size_t off = sizeof(void*); off < kScanBytes; off += sizeof(void*))
			{
				void* value = nullptr;
				if (!SafeReadPointer(static_cast<uint8_t*>(object) + off, value))
					return 0;
				if (value == needle)
					return off;
			}
			return 0;
		}

		bool InModule(const void* address, HMODULE module)
		{
			HMODULE owner = nullptr;
			return GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
			           static_cast<LPCWSTR>(address), &owner) &&
			       owner == module;
		}

		LRESULT CALLBACK DummyWndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp)
		{
			return DefWindowProcW(hwnd, msg, wp, lp);
		}
	}

	bool Install(const Callbacks& callbacks)
	{
		g_callbacks = callbacks;

		ID3D12Device* device = nullptr;
		ID3D12CommandQueue* queue = nullptr;
		IDXGIFactory4* factory = nullptr;
		IDXGISwapChain1* swapChain = nullptr;
		HWND hwnd = nullptr;
		bool ok = false;

		WNDCLASSEXW wc{sizeof(wc)};
		wc.lpfnWndProc = DummyWndProc;
		wc.hInstance = GetModuleHandleW(nullptr);
		wc.lpszClassName = L"GTAVEModLoaderProbe";
		RegisterClassExW(&wc);

		do
		{
			if (FAILED(D3D12CreateDevice(nullptr, D3D_FEATURE_LEVEL_11_0, IID_PPV_ARGS(&device))))
			{
				log::Error("ui: could not create probe D3D12 device");
				break;
			}
			D3D12_COMMAND_QUEUE_DESC qd{};
			qd.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
			if (FAILED(device->CreateCommandQueue(&qd, IID_PPV_ARGS(&queue))) || FAILED(CreateDXGIFactory1(IID_PPV_ARGS(&factory))))
			{
				log::Error("ui: could not create probe queue/factory");
				break;
			}
			hwnd = CreateWindowExW(0, wc.lpszClassName, L"", WS_OVERLAPPEDWINDOW, 0, 0, 64, 64, nullptr, nullptr, wc.hInstance, nullptr);
			DXGI_SWAP_CHAIN_DESC1 sd{};
			sd.Width = 64;
			sd.Height = 64;
			sd.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
			sd.SampleDesc.Count = 1;
			sd.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
			sd.BufferCount = 2;
			sd.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;
			if (!hwnd || FAILED(factory->CreateSwapChainForHwnd(queue, hwnd, &sd, nullptr, nullptr, &swapChain)))
			{
				log::Error("ui: could not create probe swap chain");
				break;
			}

			// The swap chain stores the queue it was created with. Depending on the DXGI version
			// it sits in the object we got back or in the inner swap chain that object wraps.
			const HMODULE dxgi = GetModuleHandleW(L"dxgi.dll");
			void* target = swapChain;
			g_queueOffset = FindPointer(swapChain, queue);
			for (size_t off = sizeof(void*); !g_queueOffset && off < kScanBytes; off += sizeof(void*))
			{
				void* inner = nullptr;
				if (!SafeReadPointer(reinterpret_cast<uint8_t*>(swapChain) + off, inner) || !inner)
					continue;
				void* innerVtable = nullptr;
				if (!SafeReadPointer(inner, innerVtable) || !InModule(innerVtable, dxgi))
					continue; // only DXGI's own swap chain objects are candidates
				if ((g_queueOffset = FindPointer(inner, queue)) != 0)
					target = inner;
			}
			if (!g_queueOffset)
			{
				log::Error("ui: command queue not found inside the swap chain");
				break;
			}

			void** vtable = *reinterpret_cast<void***>(target);
			ok = PatchSlot(vtable, kPresent, reinterpret_cast<void*>(&HookPresent), g_origPresent) &&
			     PatchSlot(vtable, kPresent1, reinterpret_cast<void*>(&HookPresent1), g_origPresent1) &&
			     PatchSlot(vtable, kResizeBuffers, reinterpret_cast<void*>(&HookResizeBuffers), g_origResizeBuffers) &&
			     PatchSlot(vtable, kResizeBuffers1, reinterpret_cast<void*>(&HookResizeBuffers1), g_origResizeBuffers1);
			if (ok)
				log::Info("ui: DXGI hooks installed (queue offset {:#x}{})", g_queueOffset, target == swapChain ? "" : ", inner swap chain");
			else
				log::Error("ui: could not patch the swap chain vtable");
		} while (false);

		// The probe objects are only needed to learn the layout; the patched vtable is shared
		// by the class, so the hooks stay active for the game's swap chain.
		t_inHook = true; // the probe swap chain must not reach our callbacks
		SafeRelease(swapChain);
		t_inHook = false;
		SafeRelease(factory);
		SafeRelease(queue);
		SafeRelease(device);
		if (hwnd)
			DestroyWindow(hwnd);
		UnregisterClassW(wc.lpszClassName, wc.hInstance);
		return ok;
	}
}
