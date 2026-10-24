#include "overlay.hpp"

#include <Windows.h>
#include <d3d12.h>
#include <dxgi1_4.h>

#include <wincodec.h>

#include <algorithm>
#include <atomic>
#include <cmath>
#include <condition_variable>
#include <cstring>
#include <format>
#include <filesystem>
#include <mutex>
#include <optional>
#include <vector>

#include <imgui.h>
#include <imgui_impl_dx12.h>
#include <imgui_impl_win32.h>

#include "../config.hpp"
#include "../log.hpp"
#include "../mods.hpp"
#include "../paths.hpp"
#include "../state.hpp"
#include "../web/browser.hpp"
#include "../web/textures.hpp"
#include "../debug/remote.hpp"
#include "../graphics/bridge.hpp"
#include "dx12_hook.hpp"
#include "menu.hpp"
#include "notify.hpp"
#include "recorder.hpp"

extern IMGUI_IMPL_API LRESULT ImGui_ImplWin32_WndProcHandler(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam);

namespace loader::ui
{
	namespace
	{
		constexpr UINT kSrvHeapSize = 64;
		constexpr UINT kMaxBackBuffers = DXGI_MAX_SWAP_CHAIN_BUFFERS;

		// Per back buffer. The back buffer itself is only held while a frame is recorded: a reference kept between
		// frames kept the game's old swap chain alive, so creating its new one (frame generation) failed (research §51).
		struct Frame
		{
			ID3D12CommandAllocator* allocator = nullptr;
			D3D12_CPU_DESCRIPTOR_HANDLE rtv{};
			UINT64 fenceValue = 0;
		};

		// Everything below is touched on the render thread only, except where noted.
		// Recursive: ImGui's Win32 handler can send messages (SetCapture, ...) that re-enter WndProc.
		std::recursive_mutex g_imguiMutex; // ImGui state is shared with the window procedure
		IDXGISwapChain3* g_swapChain = nullptr; // the game's swap chain we draw into (no reference held)
		ID3D12CommandQueue* g_queue = nullptr;  // its queue, which the ImGui backend uploads through
		ULONGLONG g_lastPresent = 0;            // when g_swapChain last presented
		std::atomic_bool g_relayout = false;    // g_swapChain is being resized: read its layout again
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

		bool AllocDescriptor(D3D12_CPU_DESCRIPTOR_HANDLE* cpu, D3D12_GPU_DESCRIPTOR_HANDLE* gpu)
		{
			const UINT inc = g_device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
			for (UINT i = 0; i < kSrvHeapSize; ++i)
				if (!g_srvUsed[i])
				{
					g_srvUsed[i] = true;
					cpu->ptr = g_srvHeap->GetCPUDescriptorHandleForHeapStart().ptr + i * inc;
					gpu->ptr = g_srvHeap->GetGPUDescriptorHandleForHeapStart().ptr + i * inc;
					return true;
				}
			return false;
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

		// One allocator and RTV slot per back buffer; the count can change when the swap chain is resized or replaced.
		bool SetFrameCount(UINT count)
		{
			count = std::min(count, kMaxBackBuffers);
			for (size_t i = count; i < g_frames.size(); ++i)
				g_frames[i].allocator->Release();
			g_frames.resize(count);
			const UINT rtvInc = g_device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_RTV);
			for (UINT i = 0; i < count; ++i)
			{
				auto& f = g_frames[i];
				f.rtv.ptr = g_rtvHeap->GetCPUDescriptorHandleForHeapStart().ptr + i * rtvInc;
				if (!f.allocator && FAILED(g_device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&f.allocator))))
					return false;
			}
			return true;
		}

		bool InitBackend(ID3D12CommandQueue* queue)
		{
			ImGui_ImplDX12_InitInfo info;
			info.Device = g_device;
			info.CommandQueue = queue;
			info.NumFramesInFlight = static_cast<int>(kMaxBackBuffers); // any back buffer count without a re-init
			info.RTVFormat = g_format;
			info.SrvDescriptorHeap = g_srvHeap;
			info.SrvDescriptorAllocFn = SrvAlloc;
			info.SrvDescriptorFreeFn = SrvFree;
			if (!ImGui_ImplDX12_Init(&info))
				return false;
			g_queue = queue;
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

			D3D12_DESCRIPTOR_HEAP_DESC rtvDesc{D3D12_DESCRIPTOR_HEAP_TYPE_RTV, kMaxBackBuffers, D3D12_DESCRIPTOR_HEAP_FLAG_NONE, 0};
			D3D12_DESCRIPTOR_HEAP_DESC srvDesc{D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV, kSrvHeapSize, D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE, 0};
			if (FAILED(g_device->CreateDescriptorHeap(&rtvDesc, IID_PPV_ARGS(&g_rtvHeap))) ||
			    FAILED(g_device->CreateDescriptorHeap(&srvDesc, IID_PPV_ARGS(&g_srvHeap))))
				return false;
			g_srvUsed.assign(kSrvHeapSize, false);

			if (!SetFrameCount(desc.BufferCount) || g_frames.empty())
				return false;
			if (FAILED(g_device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, g_frames[0].allocator, nullptr, IID_PPV_ARGS(&g_commandList))) ||
			    FAILED(g_commandList->Close()) || FAILED(g_device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&g_fence))))
				return false;
			g_fenceEvent = CreateEventW(nullptr, FALSE, FALSE, nullptr);

			std::lock_guard lock(g_imguiMutex);
			ImGui::CreateContext();
			ImGuiIO& io = ImGui::GetIO();
			io.IniFilename = nullptr; // never write imgui.ini into the game folder
			io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;
			ApplyStyle();
			LoadFonts();

			ImGui_ImplWin32_Init(g_window);
			if (!InitBackend(queue))
				return false;

			g_origWndProc = reinterpret_cast<WNDPROC>(SetWindowLongPtrW(g_window, GWLP_WNDPROC, reinterpret_cast<LONG_PTR>(&WndProc)));
			g_imguiReady = true;
			log::Info("ui: overlay ready ({} back buffers, format {})", desc.BufferCount, static_cast<int>(g_format));
			return true;
		}

		// After the swap chain was resized, or the game replaced it (e.g. when frame generation is switched on):
		// reads its back buffer count and format again.
		bool Attach(IDXGISwapChain3* swapChain, ID3D12CommandQueue* queue)
		{
			DXGI_SWAP_CHAIN_DESC desc{};
			if (FAILED(swapChain->GetDesc(&desc)) || desc.OutputWindow != g_window)
				return false;
			WaitForGpu();
			if (desc.BufferDesc.Format != g_format || queue != g_queue)
			{
				// The backend's pipeline is built for one format, and it uploads textures through the queue.
				std::lock_guard lock(g_imguiMutex);
				ImGui_ImplDX12_Shutdown();
				g_format = desc.BufferDesc.Format;
				if (!InitBackend(queue))
				{
					g_failed = true;
					log::Error("ui: overlay could not follow the game's new swap chain; the loader keeps running without UI");
					return false;
				}
			}
			if (!SetFrameCount(desc.BufferCount) || g_frames.empty())
			{
				g_failed = true;
				log::Error("ui: overlay could not follow the game's new swap chain; the loader keeps running without UI");
				return false;
			}
			if (swapChain != g_swapChain)
				log::Info("ui: overlay moved to the game's new swap chain ({} back buffers, format {})", desc.BufferCount,
				    static_cast<int>(g_format));
			g_swapChain = swapChain;
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

			const UINT index = g_swapChain->GetCurrentBackBufferIndex();
			if (index >= g_frames.size())
				return;
			Frame& f = g_frames[index];
			ID3D12Resource* backBuffer = nullptr;
			if (FAILED(g_swapChain->GetBuffer(index, IID_PPV_ARGS(&backBuffer))))
				return;
			if (g_fence->GetCompletedValue() < f.fenceValue)
			{
				g_fence->SetEventOnCompletion(f.fenceValue, g_fenceEvent);
				WaitForSingleObject(g_fenceEvent, 1000);
			}
			D3D12_RENDER_TARGET_VIEW_DESC rtvDesc{};
			rtvDesc.Format = g_format;
			rtvDesc.ViewDimension = D3D12_RTV_DIMENSION_TEXTURE2D;
			g_device->CreateRenderTargetView(backBuffer, &rtvDesc, f.rtv);
			f.allocator->Reset();
			g_commandList->Reset(f.allocator, nullptr);
			web::textures::OnRender(g_device, g_commandList, backBuffer, g_fenceCounter + 1, g_fence->GetCompletedValue());

			D3D12_RESOURCE_BARRIER barrier{};
			barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
			barrier.Transition.pResource = backBuffer;
			barrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
			barrier.Transition.StateBefore = D3D12_RESOURCE_STATE_PRESENT;
			barrier.Transition.StateAfter = D3D12_RESOURCE_STATE_RENDER_TARGET;
			g_commandList->ResourceBarrier(1, &barrier);
			g_commandList->OMSetRenderTargets(1, &f.rtv, FALSE, nullptr);
			g_commandList->SetDescriptorHeaps(1, &g_srvHeap);
			web::Upload(g_device, g_commandList, index, static_cast<unsigned>(g_frames.size()), &AllocDescriptor);
			ImGui_ImplDX12_RenderDrawData(drawData, g_commandList);
			std::swap(barrier.Transition.StateBefore, barrier.Transition.StateAfter);
			g_commandList->ResourceBarrier(1, &barrier);
			g_commandList->Close();

			ID3D12CommandList* lists[] = {g_commandList};
			queue->ExecuteCommandLists(1, lists);
			queue->Signal(g_fence, ++g_fenceCounter);
			f.fenceValue = g_fenceCounter;
			backBuffer->Release(); // the swap chain keeps it alive while the GPU draws; it waits for the queue before letting go
		}

		// Development aid: while ModLoader\debug_fps.txt exists, log the frame rate and the slowest frame every 5 s.
		void FrameStats()
		{
			static uint64_t nextCheck = 0, windowStart = 0;
			static bool on = false;
			static int frames = 0;
			static double worst = 0;
			const uint64_t now = GetTickCount64();
			if (now >= nextCheck)
			{
				nextCheck = now + 2000;
				std::error_code ec;
				on = std::filesystem::exists(paths::Get().root / L"debug_fps.txt", ec);
			}
			if (!on)
			{
				windowStart = 0;
				return;
			}
			LARGE_INTEGER freq, t;
			QueryPerformanceFrequency(&freq);
			QueryPerformanceCounter(&t);
			static int64_t prev = 0;
			if (prev)
			{
				const double ms = static_cast<double>(t.QuadPart - prev) * 1000.0 / static_cast<double>(freq.QuadPart);
				worst = std::max(worst, ms);
				// (a long frame, with the time of day: to line up with the renderer's log)
				if (ms > 100.0 && windowStart)
				{
					SYSTEMTIME now{};
					GetLocalTime(&now);
					log::Info("fps: a {:.0f} ms frame at {:02}:{:02}:{:02}.{:03}", ms, now.wHour, now.wMinute, now.wSecond, now.wMilliseconds);
				}
			}
			prev = t.QuadPart;
			if (!windowStart)
			{
				windowStart = now;
				frames = 0;
				worst = 0;
				return;
			}
			++frames;
			if (now - windowStart >= 5000)
			{
				log::Info("fps: {:.1f}, slowest frame {:.1f} ms", frames * 1000.0 / static_cast<double>(now - windowStart), worst);
				windowStart = now;
				frames = 0;
				worst = 0;
			}
		}

		void TakeShot();

		void OnPresent(IDXGISwapChain3* swapChain, ID3D12CommandQueue* queue)
		{
			FrameStats();
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
			const ULONGLONG now = GetTickCount64();
			if (swapChain != g_swapChain || g_relayout || now - g_lastPresent > 500)
			{
				// Another swap chain takes over only once ours has stopped presenting: two presenting side by side
				// keep the first.
				if (swapChain != g_swapChain && now - g_lastPresent <= 500)
					return;
				g_relayout = false;
				if (!Attach(swapChain, queue))
					return;
			}
			g_lastPresent = now;
			Render(queue);
			TakeShot();
			recorder::OnPresent(swapChain, queue);
			graphics::bridge::OnPresent(queue);
		}

		// ---- screenshots (debug/remote) --------------------------------------------------------

		std::mutex g_shotMutex;
		std::condition_variable g_shotDone;
		std::wstring g_shotPath; // the request under way ("" = none)
		uint64_t g_shotId = 0, g_shotFinished = 0;
		std::string g_shotError;
		ID3D12CommandAllocator* g_shotAllocator = nullptr;
		ID3D12GraphicsCommandList* g_shotList = nullptr;

		float Half(uint16_t h)
		{
			const uint32_t sign = (h & 0x8000u) << 16, exp = (h >> 10) & 0x1F, man = h & 0x3FF;
			uint32_t bits;
			if (exp == 0)
				bits = sign; // (denormals as zero)
			else if (exp == 31)
				bits = sign | 0x7F800000u | (man << 13);
			else
				bits = sign | ((exp + 112) << 23) | (man << 13);
			float f;
			std::memcpy(&f, &bits, 4);
			return f;
		}

		// One row of the back buffer as BGRA bytes; false for a format we do not read.
		bool ToBgra(DXGI_FORMAT format, const uint8_t* src, uint8_t* dst, UINT width)
		{
			for (UINT x = 0; x < width; ++x, dst += 4)
			{
				switch (format)
				{
				case DXGI_FORMAT_R8G8B8A8_UNORM:
				case DXGI_FORMAT_R8G8B8A8_UNORM_SRGB:
					dst[0] = src[x * 4 + 2], dst[1] = src[x * 4 + 1], dst[2] = src[x * 4], dst[3] = 255;
					break;
				case DXGI_FORMAT_B8G8R8A8_UNORM:
				case DXGI_FORMAT_B8G8R8A8_UNORM_SRGB:
					dst[0] = src[x * 4], dst[1] = src[x * 4 + 1], dst[2] = src[x * 4 + 2], dst[3] = 255;
					break;
				case DXGI_FORMAT_R10G10B10A2_UNORM:
				{
					uint32_t p;
					std::memcpy(&p, src + x * 4, 4);
					dst[2] = static_cast<uint8_t>((p & 0x3FF) >> 2);
					dst[1] = static_cast<uint8_t>(((p >> 10) & 0x3FF) >> 2);
					dst[0] = static_cast<uint8_t>(((p >> 20) & 0x3FF) >> 2);
					dst[3] = 255;
					break;
				}
				case DXGI_FORMAT_R16G16B16A16_FLOAT:
				{
					// scRGB (linear, 1.0 = SDR white) to 8-bit sRGB, clipped.
					uint16_t c[4];
					std::memcpy(c, src + x * 8, 8);
					for (int k = 0; k < 3; ++k)
					{
						const float v = std::clamp(Half(c[k]), 0.0f, 1.0f);
						const float s = v <= 0.0031308f ? v * 12.92f : 1.055f * std::pow(v, 1.0f / 2.4f) - 0.055f;
						dst[2 - k] = static_cast<uint8_t>(std::clamp(s, 0.0f, 1.0f) * 255.0f + 0.5f);
					}
					dst[3] = 255;
					break;
				}
				default:
					return false;
				}
			}
			return true;
		}

		bool SavePng(const std::wstring& path, std::vector<uint8_t>& bgra, UINT width, UINT height, std::string& error)
		{
			const HRESULT co = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
			IWICImagingFactory* factory = nullptr;
			IWICStream* stream = nullptr;
			IWICBitmapEncoder* encoder = nullptr;
			IWICBitmapFrameEncode* frame = nullptr;
			bool ok = false;
			if (SUCCEEDED(CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&factory))) &&
			    SUCCEEDED(factory->CreateStream(&stream)) && SUCCEEDED(stream->InitializeFromFilename(path.c_str(), GENERIC_WRITE)) &&
			    SUCCEEDED(factory->CreateEncoder(GUID_ContainerFormatPng, nullptr, &encoder)) &&
			    SUCCEEDED(encoder->Initialize(stream, WICBitmapEncoderNoCache)) && SUCCEEDED(encoder->CreateNewFrame(&frame, nullptr)) &&
			    SUCCEEDED(frame->Initialize(nullptr)) && SUCCEEDED(frame->SetSize(width, height)))
			{
				WICPixelFormatGUID fmt = GUID_WICPixelFormat32bppBGRA;
				ok = SUCCEEDED(frame->SetPixelFormat(&fmt)) && IsEqualGUID(fmt, GUID_WICPixelFormat32bppBGRA) &&
				     SUCCEEDED(frame->WritePixels(height, width * 4, static_cast<UINT>(bgra.size()), bgra.data())) && SUCCEEDED(frame->Commit()) &&
				     SUCCEEDED(encoder->Commit());
			}
			if (frame)
				frame->Release();
			if (encoder)
				encoder->Release();
			if (stream)
				stream->Release();
			if (factory)
				factory->Release();
			if (SUCCEEDED(co))
				CoUninitialize();
			if (!ok)
				error = "could not write the PNG";
			return ok;
		}

		// The frame about to be presented (the game's picture with the overlay on it), copied out and saved.
		void TakeShot()
		{
			std::wstring path;
			uint64_t id;
			{
				std::lock_guard lock(g_shotMutex);
				if (g_shotPath.empty())
					return;
				path = g_shotPath;
				id = g_shotId;
			}
			std::string error;
			ID3D12Resource* backBuffer = nullptr;
			ID3D12Resource* readback = nullptr;
			if (!g_shotAllocator &&
			    (FAILED(g_device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&g_shotAllocator))) ||
			        FAILED(g_device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, g_shotAllocator, nullptr, IID_PPV_ARGS(&g_shotList))) ||
			        FAILED(g_shotList->Close())))
				error = "no command list";
			else if (FAILED(g_swapChain->GetBuffer(g_swapChain->GetCurrentBackBufferIndex(), IID_PPV_ARGS(&backBuffer))))
				error = "no back buffer";
			else
			{
				const D3D12_RESOURCE_DESC desc = backBuffer->GetDesc();
				D3D12_PLACED_SUBRESOURCE_FOOTPRINT fp{};
				UINT rows = 0;
				UINT64 rowSize = 0, total = 0;
				g_device->GetCopyableFootprints(&desc, 0, 1, 0, &fp, &rows, &rowSize, &total);
				D3D12_HEAP_PROPERTIES heap{};
				heap.Type = D3D12_HEAP_TYPE_READBACK;
				D3D12_RESOURCE_DESC buf{};
				buf.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
				buf.Width = total;
				buf.Height = 1;
				buf.DepthOrArraySize = 1;
				buf.MipLevels = 1;
				buf.SampleDesc.Count = 1;
				buf.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
				if (FAILED(g_device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &buf, D3D12_RESOURCE_STATE_COPY_DEST, nullptr, IID_PPV_ARGS(&readback))))
					error = "no readback buffer";
				else
				{
					g_shotAllocator->Reset();
					g_shotList->Reset(g_shotAllocator, nullptr);
					D3D12_RESOURCE_BARRIER barrier{};
					barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
					barrier.Transition.pResource = backBuffer;
					barrier.Transition.Subresource = 0;
					barrier.Transition.StateBefore = D3D12_RESOURCE_STATE_PRESENT;
					barrier.Transition.StateAfter = D3D12_RESOURCE_STATE_COPY_SOURCE;
					g_shotList->ResourceBarrier(1, &barrier);
					D3D12_TEXTURE_COPY_LOCATION dst{};
					dst.pResource = readback;
					dst.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
					dst.PlacedFootprint = fp;
					D3D12_TEXTURE_COPY_LOCATION src{};
					src.pResource = backBuffer;
					src.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
					src.SubresourceIndex = 0;
					g_shotList->CopyTextureRegion(&dst, 0, 0, 0, &src, nullptr);
					std::swap(barrier.Transition.StateBefore, barrier.Transition.StateAfter);
					g_shotList->ResourceBarrier(1, &barrier);
					g_shotList->Close();
					ID3D12CommandList* lists[] = {g_shotList};
					g_queue->ExecuteCommandLists(1, lists);
					g_queue->Signal(g_fence, ++g_fenceCounter);
					if (g_fence->GetCompletedValue() < g_fenceCounter)
					{
						g_fence->SetEventOnCompletion(g_fenceCounter, g_fenceEvent);
						WaitForSingleObject(g_fenceEvent, 2000);
					}
					const UINT width = static_cast<UINT>(desc.Width), height = desc.Height;
					std::vector<uint8_t> bgra(static_cast<size_t>(width) * height * 4);
					uint8_t* mapped = nullptr;
					D3D12_RANGE range{0, static_cast<SIZE_T>(total)};
					if (FAILED(readback->Map(0, &range, reinterpret_cast<void**>(&mapped))))
						error = "could not read the frame";
					else
					{
						for (UINT y = 0; y < height && error.empty(); ++y)
							if (!ToBgra(desc.Format, mapped + fp.Offset + static_cast<size_t>(y) * fp.Footprint.RowPitch,
							        bgra.data() + static_cast<size_t>(y) * width * 4, width))
								error = std::format("back buffer format {} not supported", static_cast<int>(desc.Format));
						D3D12_RANGE none{0, 0};
						readback->Unmap(0, &none);
						if (error.empty())
							SavePng(path, bgra, width, height, error);
					}
				}
			}
			if (readback)
				readback->Release();
			if (backBuffer)
				backBuffer->Release();
			std::lock_guard lock(g_shotMutex);
			if (g_shotId == id)
			{
				g_shotPath.clear();
				g_shotError = error;
				g_shotFinished = id;
				g_shotDone.notify_all();
			}
		}

		void OnReleaseBuffers(IDXGISwapChain3* swapChain)
		{
			if (swapChain != g_swapChain || !g_imguiReady)
				return;
			WaitForGpu(); // our command lists still draw into the buffers being resized
			g_relayout = true;
		}

		bool IsInputMessage(UINT msg)
		{
			return (msg >= WM_KEYFIRST && msg <= WM_KEYLAST) || (msg >= WM_MOUSEFIRST && msg <= WM_MOUSELAST) || msg == WM_INPUT;
		}

		LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp)
		{
			// A key a tester sent (debug/remote): the mods' hotkeys only, the game never sees it.
			if (msg == debug::remote::kKeyMessage)
			{
				if (!g_capture && state::story && !state::online)
				{
					mods::OnKeyDown(static_cast<uint32_t>(wp));
					graphics::bridge::OnKeyDown(static_cast<uint32_t>(wp));
				}
				return 0;
			}
			if (msg == WM_KEYDOWN && !(lp & (1 << 30)) && wp == MenuKey())
			{
				ToggleMenu();
				return 0;
			}
			if ((msg == WM_KEYDOWN || msg == WM_SYSKEYDOWN) && !(lp & (1 << 30)))
			{
				if (CaptureHotkey(static_cast<unsigned>(wp)))
					return 0;
				// Mod hotkeys: story mode only, and not while the loader UI owns the keyboard.
				if (!g_capture && state::story && !state::online)
				{
					mods::OnKeyDown(static_cast<uint32_t>(wp));
					graphics::bridge::OnKeyDown(static_cast<uint32_t>(wp));
				}
			}
			// The web browser gets the page area's input (its toolbar is ImGui's).
			if (!MenuOpen())
				web::OnMessage(hwnd, msg, wp, lp);
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

	bool Capture(const std::wstring& path, std::string& error)
	{
		std::unique_lock lock(g_shotMutex);
		if (!g_imguiReady)
		{
			error = "the game has not drawn a frame yet";
			return false;
		}
		if (!g_shotPath.empty())
		{
			error = "another screenshot is under way";
			return false;
		}
		g_shotPath = path;
		const uint64_t id = ++g_shotId;
		if (!g_shotDone.wait_for(lock, std::chrono::seconds(5), [&] { return g_shotFinished == id; }))
		{
			g_shotPath.clear();
			error = "no frame within 5 s (is the game drawing?)";
			return false;
		}
		error = g_shotError;
		return error.empty();
	}

	ID3D12Device* Device()
	{
		return g_device;
	}
}
