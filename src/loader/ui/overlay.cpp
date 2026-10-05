#include "overlay.hpp"

#include <Windows.h>
#include <d3d12.h>
#include <dxgi1_4.h>

#include <atomic>
#include <mutex>
#include <optional>
#include <vector>

#include <imgui.h>
#include <imgui_impl_dx12.h>
#include <imgui_impl_win32.h>

#include "../config.hpp"
#include "../log.hpp"
#include "dx12_hook.hpp"
#include "menu.hpp"

extern IMGUI_IMPL_API LRESULT ImGui_ImplWin32_WndProcHandler(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam);

namespace loader::ui
{
	namespace
	{
		constexpr UINT kSrvHeapSize = 64;

		struct Frame
		{
			ID3D12CommandAllocator* allocator = nullptr;
			ID3D12Resource* backBuffer = nullptr;
			D3D12_CPU_DESCRIPTOR_HANDLE rtv{};
			UINT64 fenceValue = 0;
		};

		// Everything below is touched on the render thread only, except where noted.
		// Recursive: ImGui's Win32 handler can send messages (SetCapture, ...) that re-enter WndProc.
		std::recursive_mutex g_imguiMutex; // ImGui state is shared with the window procedure
		IDXGISwapChain3* g_swapChain = nullptr; // the game's swap chain we draw into
		ID3D12Device* g_device = nullptr;
		ID3D12DescriptorHeap* g_rtvHeap = nullptr;
		ID3D12DescriptorHeap* g_srvHeap = nullptr;
		ID3D12GraphicsCommandList* g_commandList = nullptr;
		ID3D12Fence* g_fence = nullptr;
		HANDLE g_fenceEvent = nullptr;
		UINT64 g_fenceCounter = 0;
		std::vector<Frame> g_frames;
		DXGI_FORMAT g_format = DXGI_FORMAT_UNKNOWN;
		std::vector<bool> g_srvUsed;
		bool g_imguiReady = false;
		bool g_failed = false;

		HWND g_window = nullptr;
		WNDPROC g_origWndProc = nullptr;
		std::atomic_bool g_capture = false;

		// ---- SRV descriptor allocator for the ImGui DX12 backend -------------------------------

		void SrvAlloc(ImGui_ImplDX12_InitInfo*, D3D12_CPU_DESCRIPTOR_HANDLE* cpu, D3D12_GPU_DESCRIPTOR_HANDLE* gpu)
		{
			const UINT inc = g_device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
			for (UINT i = 0; i < kSrvHeapSize; ++i)
			{
				if (g_srvUsed[i])
					continue;
				g_srvUsed[i] = true;
				cpu->ptr = g_srvHeap->GetCPUDescriptorHandleForHeapStart().ptr + i * inc;
				gpu->ptr = g_srvHeap->GetGPUDescriptorHandleForHeapStart().ptr + i * inc;
				return;
			}
			log::Error("ui: SRV descriptor heap exhausted");
			*cpu = g_srvHeap->GetCPUDescriptorHandleForHeapStart();
			*gpu = g_srvHeap->GetGPUDescriptorHandleForHeapStart();
		}

		void SrvFree(ImGui_ImplDX12_InitInfo*, D3D12_CPU_DESCRIPTOR_HANDLE cpu, D3D12_GPU_DESCRIPTOR_HANDLE)
		{
			const UINT inc = g_device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
			const UINT i = static_cast<UINT>((cpu.ptr - g_srvHeap->GetCPUDescriptorHandleForHeapStart().ptr) / inc);
			if (i < kSrvHeapSize)
				g_srvUsed[i] = false;
		}

		// ---- GPU resources ---------------------------------------------------------------------

		void WaitForGpu()
		{
			if (!g_fence)
				return;
			for (auto& f : g_frames)
			{
				if (g_fence->GetCompletedValue() < f.fenceValue)
				{
					g_fence->SetEventOnCompletion(f.fenceValue, g_fenceEvent);
					WaitForSingleObject(g_fenceEvent, 2000);
				}
			}
		}

		void ReleaseBackBuffers()
		{
			WaitForGpu();
			for (auto& f : g_frames)
			{
				if (f.backBuffer)
					f.backBuffer->Release();
				f.backBuffer = nullptr;
			}
		}

		bool CreateBackBuffers()
		{
			const UINT rtvInc = g_device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_RTV);
			for (UINT i = 0; i < g_frames.size(); ++i)
			{
				auto& f = g_frames[i];
				if (FAILED(g_swapChain->GetBuffer(i, IID_PPV_ARGS(&f.backBuffer))))
					return false;
				f.rtv.ptr = g_rtvHeap->GetCPUDescriptorHandleForHeapStart().ptr + i * rtvInc;
				D3D12_RENDER_TARGET_VIEW_DESC rtvDesc{};
				rtvDesc.Format = g_format;
				rtvDesc.ViewDimension = D3D12_RTV_DIMENSION_TEXTURE2D;
				g_device->CreateRenderTargetView(f.backBuffer, &rtvDesc, f.rtv);
			}
			return true;
		}

		LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp);

		bool Init(IDXGISwapChain3* swapChain, ID3D12CommandQueue* queue)
		{
			DXGI_SWAP_CHAIN_DESC desc{};
			if (FAILED(swapChain->GetDesc(&desc)) || FAILED(swapChain->GetDevice(IID_PPV_ARGS(&g_device))))
				return false;
			g_swapChain = swapChain;
			g_format = desc.BufferDesc.Format;
			g_window = desc.OutputWindow;
			g_frames.resize(desc.BufferCount);

			D3D12_DESCRIPTOR_HEAP_DESC rtvDesc{D3D12_DESCRIPTOR_HEAP_TYPE_RTV, desc.BufferCount, D3D12_DESCRIPTOR_HEAP_FLAG_NONE, 0};
			D3D12_DESCRIPTOR_HEAP_DESC srvDesc{D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV, kSrvHeapSize, D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE, 0};
			if (FAILED(g_device->CreateDescriptorHeap(&rtvDesc, IID_PPV_ARGS(&g_rtvHeap))) ||
			    FAILED(g_device->CreateDescriptorHeap(&srvDesc, IID_PPV_ARGS(&g_srvHeap))))
				return false;
			g_srvUsed.assign(kSrvHeapSize, false);

			for (auto& f : g_frames)
				if (FAILED(g_device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&f.allocator))))
					return false;
			if (FAILED(g_device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, g_frames[0].allocator, nullptr, IID_PPV_ARGS(&g_commandList))) ||
			    FAILED(g_commandList->Close()) || FAILED(g_device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&g_fence))))
				return false;
			g_fenceEvent = CreateEventW(nullptr, FALSE, FALSE, nullptr);
			if (!CreateBackBuffers())
				return false;

			std::lock_guard lock(g_imguiMutex);
			ImGui::CreateContext();
			ImGuiIO& io = ImGui::GetIO();
			io.IniFilename = nullptr; // never write imgui.ini into the game folder
			io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;
			ApplyStyle();
			LoadFonts();

			ImGui_ImplWin32_Init(g_window);
			ImGui_ImplDX12_InitInfo info;
			info.Device = g_device;
			info.CommandQueue = queue;
			info.NumFramesInFlight = static_cast<int>(desc.BufferCount);
			info.RTVFormat = g_format;
			info.SrvDescriptorHeap = g_srvHeap;
			info.SrvDescriptorAllocFn = SrvAlloc;
			info.SrvDescriptorFreeFn = SrvFree;
			if (!ImGui_ImplDX12_Init(&info))
				return false;

			g_origWndProc = reinterpret_cast<WNDPROC>(SetWindowLongPtrW(g_window, GWLP_WNDPROC, reinterpret_cast<LONG_PTR>(&WndProc)));
			g_imguiReady = true;
			log::Info("ui: overlay ready ({} back buffers, format {})", desc.BufferCount, static_cast<int>(g_format));
			return true;
		}

		void Render(ID3D12CommandQueue* queue)
		{
			{
				std::lock_guard lock(g_imguiMutex);
				ImGui_ImplDX12_NewFrame();
				ImGui_ImplWin32_NewFrame();
				ImGui::NewFrame();
				DrawFrame();
				g_capture = WantsInput();
				ImGui::GetIO().MouseDrawCursor = g_capture.load();
				ImGui::Render();
			}

			ImDrawData* drawData = ImGui::GetDrawData();
			if (!drawData || drawData->CmdLists.Size == 0)
				return;

			Frame& f = g_frames[g_swapChain->GetCurrentBackBufferIndex()];
			if (!f.backBuffer)
				return;
			if (g_fence->GetCompletedValue() < f.fenceValue)
			{
				g_fence->SetEventOnCompletion(f.fenceValue, g_fenceEvent);
				WaitForSingleObject(g_fenceEvent, 1000);
			}
			f.allocator->Reset();
			g_commandList->Reset(f.allocator, nullptr);

			D3D12_RESOURCE_BARRIER barrier{};
			barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
			barrier.Transition.pResource = f.backBuffer;
			barrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
			barrier.Transition.StateBefore = D3D12_RESOURCE_STATE_PRESENT;
			barrier.Transition.StateAfter = D3D12_RESOURCE_STATE_RENDER_TARGET;
			g_commandList->ResourceBarrier(1, &barrier);
			g_commandList->OMSetRenderTargets(1, &f.rtv, FALSE, nullptr);
			g_commandList->SetDescriptorHeaps(1, &g_srvHeap);
			ImGui_ImplDX12_RenderDrawData(drawData, g_commandList);
			std::swap(barrier.Transition.StateBefore, barrier.Transition.StateAfter);
			g_commandList->ResourceBarrier(1, &barrier);
			g_commandList->Close();

			ID3D12CommandList* lists[] = {g_commandList};
			queue->ExecuteCommandLists(1, lists);
			queue->Signal(g_fence, ++g_fenceCounter);
			f.fenceValue = g_fenceCounter;
		}

		void OnPresent(IDXGISwapChain3* swapChain, ID3D12CommandQueue* queue)
		{
			if (g_failed)
				return;
			if (!g_imguiReady)
			{
				DXGI_SWAP_CHAIN_DESC desc{};
				// Only attach to a swap chain that presents into a visible game window.
				if (FAILED(swapChain->GetDesc(&desc)) || !IsWindowVisible(desc.OutputWindow))
					return;
				if (!Init(swapChain, queue))
				{
					g_failed = true;
					log::Error("ui: overlay initialisation failed; the loader keeps running without UI");
					return;
				}
			}
			if (swapChain != g_swapChain)
				return;
			if (!g_frames[0].backBuffer && !CreateBackBuffers())
				return;
			Render(queue);
		}

		void OnReleaseBuffers(IDXGISwapChain3* swapChain)
		{
			if (swapChain == g_swapChain && g_imguiReady)
				ReleaseBackBuffers(); // recreated lazily on the next Present
		}

		bool IsInputMessage(UINT msg)
		{
			return (msg >= WM_KEYFIRST && msg <= WM_KEYLAST) || (msg >= WM_MOUSEFIRST && msg <= WM_MOUSELAST) || msg == WM_INPUT;
		}

		LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp)
		{
			if (msg == WM_KEYDOWN && !(lp & (1 << 30)) && wp == MenuKey())
			{
				ToggleMenu();
				return 0;
			}
			{
				std::lock_guard lock(g_imguiMutex);
				ImGui_ImplWin32_WndProcHandler(hwnd, msg, wp, lp);
			}
			// While the loader UI is up, the game must not see keyboard/mouse input.
			if (g_capture && IsInputMessage(msg))
				return msg == WM_INPUT ? DefWindowProcW(hwnd, msg, wp, lp) : 0;
			return CallWindowProcW(g_origWndProc, hwnd, msg, wp, lp);
		}
	}

	bool StartOverlay()
	{
		// Started early when legacy packs are converted (to show progress on the loading screen), and again
		// on the first script tick: install once.
		static std::mutex mutex;
		static std::optional<bool> installed;
		std::lock_guard lock(mutex);
		if (!installed)
			installed = dx12::Install({&OnPresent, &OnReleaseBuffers});
		return *installed;
	}

	bool CapturesInput()
	{
		return g_capture;
	}

	HWND GameWindow()
	{
		return g_window;
	}

	ID3D12Device* Device()
	{
		return g_device;
	}
}
