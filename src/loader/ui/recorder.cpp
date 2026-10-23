#include "recorder.hpp"

#include <Windows.h>
#include <d3d12.h>
#include <dxgi1_4.h>

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <thread>
#include <vector>

#include "../log.hpp"

namespace loader::ui::recorder
{
	namespace
	{
		// The file: "GREC", version, the scale; then each frame (a 1, its time in seconds from the start as a double, its
		// width and height, the four words it was tagged with, its pixels: BGR rows top down), and at the end every present's
		// time (a 2, the count, the doubles).
		constexpr int kSlots = 8;

		struct Slot
		{
			ID3D12CommandAllocator* allocator = nullptr;
			ID3D12GraphicsCommandList* list = nullptr;
			ID3D12Resource* buffer = nullptr;
			UINT64 size = 0, fence = 0;
			double time = 0;
			D3D12_PLACED_SUBRESOURCE_FOOTPRINT footprint{};
			UINT width = 0, height = 0;
			DXGI_FORMAT format = DXGI_FORMAT_UNKNOWN;
			uint32_t info[4]{};
			std::atomic<int> state = 0; // 0 free, 1 copied on the GPU (the worker writes it next)
		};

		std::mutex g_mutex; // Start against the render thread
		Slot g_slots[kSlots];
		ID3D12Device* g_device = nullptr;
		std::atomic<ID3D12Fence*> g_fence = nullptr;
		UINT64 g_fenceValue = 0;
		std::atomic_bool g_taking = false, g_busy = false;
		std::wstring g_path;
		double g_seconds = 0, g_interval = 0, g_lastTaken = -1;
		int g_scale = 2;
		LARGE_INTEGER g_start{}, g_frequency{};
		uint64_t g_dropped = 0;
		std::atomic<uint64_t> g_taken = 0, g_written = 0;
		std::vector<double> g_presents;
		std::atomic<uint32_t> g_info[4]{};

		double Now()
		{
			LARGE_INTEGER t;
			QueryPerformanceCounter(&t);
			return static_cast<double>(t.QuadPart - g_start.QuadPart) / static_cast<double>(g_frequency.QuadPart);
		}

		uint8_t Srgb(float v)
		{
			v = std::clamp(v, 0.0f, 1.0f);
			const float s = v <= 0.0031308f ? v * 12.92f : 1.055f * std::pow(v, 1.0f / 2.4f) - 0.055f;
			return static_cast<uint8_t>(s * 255.0f + 0.5f);
		}

		float Half(uint16_t h)
		{
			const uint32_t sign = (h & 0x8000u) << 16, exp = (h >> 10) & 0x1F, man = h & 0x3FF;
			const uint32_t bits = exp == 0 ? sign : exp == 31 ? sign | 0x7F800000u | (man << 13) : sign | ((exp + 112) << 23) | (man << 13);
			float f;
			std::memcpy(&f, &bits, 4);
			return f;
		}

		// One pixel as blue, green, red (0-255); false for a format not read.
		bool Pixel(DXGI_FORMAT format, const uint8_t* p, int& b, int& g, int& r)
		{
			switch (format)
			{
			case DXGI_FORMAT_R8G8B8A8_UNORM:
			case DXGI_FORMAT_R8G8B8A8_UNORM_SRGB:
				r = p[0], g = p[1], b = p[2];
				return true;
			case DXGI_FORMAT_B8G8R8A8_UNORM:
			case DXGI_FORMAT_B8G8R8A8_UNORM_SRGB:
				b = p[0], g = p[1], r = p[2];
				return true;
			case DXGI_FORMAT_R10G10B10A2_UNORM:
			{
				uint32_t v;
				std::memcpy(&v, p, 4);
				r = static_cast<int>((v & 0x3FF) >> 2), g = static_cast<int>(((v >> 10) & 0x3FF) >> 2), b = static_cast<int>(((v >> 20) & 0x3FF) >> 2);
				return true;
			}
			case DXGI_FORMAT_R16G16B16A16_FLOAT:
			{
				uint16_t c[4];
				std::memcpy(c, p, 8);
				r = Srgb(Half(c[0])), g = Srgb(Half(c[1])), b = Srgb(Half(c[2]));
				return true;
			}
			default:
				return false;
			}
		}

		// The worker: each copied frame in turn, scaled down and written; then the presents; then the ring let go.
		void Write()
		{
			std::ofstream out(std::filesystem::path(g_path), std::ios::binary | std::ios::trunc);
			const uint32_t header[3] = {0x43455247, 1, static_cast<uint32_t>(g_scale)}; // "GREC"
			out.write(reinterpret_cast<const char*>(header), sizeof(header));
			HANDLE event = CreateEventW(nullptr, FALSE, FALSE, nullptr);
			std::vector<uint8_t> row;
			std::vector<int> sum;
			bool unsupported = false;
			for (;;)
			{
				Slot& s = g_slots[g_written % kSlots];
				if (s.state.load() != 1)
				{
					if (!g_taking && g_written == g_taken)
						break;
					// (the game stopped presenting: the time is up all the same)
					if (Now() > g_seconds + 2.0)
						g_taking = false;
					Sleep(1);
					continue;
				}
				if (ID3D12Fence* fence = g_fence; fence->GetCompletedValue() < s.fence)
				{
					fence->SetEventOnCompletion(s.fence, event);
					WaitForSingleObject(event, 2000);
				}
				uint8_t* mapped = nullptr;
				D3D12_RANGE range{0, static_cast<SIZE_T>(s.size)};
				if (SUCCEEDED(s.buffer->Map(0, &range, reinterpret_cast<void**>(&mapped))))
				{
					const int k = g_scale;
					const UINT w = s.width / k, h = s.height / k;
					const UINT bytes = s.format == DXGI_FORMAT_R16G16B16A16_FLOAT ? 8 : 4;
					const uint32_t kind = 1, size[2] = {w, h};
					out.write(reinterpret_cast<const char*>(&kind), 4);
					out.write(reinterpret_cast<const char*>(&s.time), 8);
					out.write(reinterpret_cast<const char*>(size), 8);
					out.write(reinterpret_cast<const char*>(s.info), 16);
					row.resize(static_cast<size_t>(w) * 3);
					sum.assign(static_cast<size_t>(w) * 3, 0);
					for (UINT y = 0; y < h; ++y)
					{
						std::fill(sum.begin(), sum.end(), 0);
						for (int dy = 0; dy < k; ++dy)
						{
							const uint8_t* src = mapped + s.footprint.Offset + static_cast<size_t>(y * k + dy) * s.footprint.Footprint.RowPitch;
							for (UINT x = 0; x < w; ++x)
								for (int dx = 0; dx < k; ++dx)
								{
									int b = 0, g = 0, r = 0;
									if (!Pixel(s.format, src + static_cast<size_t>(x * k + dx) * bytes, b, g, r))
										unsupported = true;
									sum[x * 3] += b, sum[x * 3 + 1] += g, sum[x * 3 + 2] += r;
								}
						}
						for (size_t i = 0; i < row.size(); ++i)
							row[i] = static_cast<uint8_t>(sum[i] / (k * k));
						out.write(reinterpret_cast<const char*>(row.data()), static_cast<std::streamsize>(row.size()));
					}
					D3D12_RANGE none{0, 0};
					s.buffer->Unmap(0, &none);
				}
				s.state = 0;
				++g_written;
			}
			CloseHandle(event);
			{
				std::lock_guard lock(g_mutex);
				const uint32_t count[2] = {2, static_cast<uint32_t>(g_presents.size())};
				out.write(reinterpret_cast<const char*>(count), sizeof(count));
				out.write(reinterpret_cast<const char*>(g_presents.data()), static_cast<std::streamsize>(g_presents.size() * sizeof(double)));
				log::Info("recorder: {} frames written, {} left out (the ring full), {} presents{}", g_written.load(), g_dropped, g_presents.size(),
				    unsupported ? "; the back buffer's format is not read (black)" : "");
				for (Slot& s : g_slots)
				{
					if (s.list)
						s.list->Release();
					if (s.allocator)
						s.allocator->Release();
					if (s.buffer)
						s.buffer->Release();
					s.list = nullptr, s.allocator = nullptr, s.buffer = nullptr, s.size = 0;
				}
			}
			g_busy = false;
		}

		bool Prepare(Slot& s, const D3D12_RESOURCE_DESC& desc)
		{
			D3D12_PLACED_SUBRESOURCE_FOOTPRINT footprint{};
			UINT rows = 0;
			UINT64 rowSize = 0, total = 0;
			g_device->GetCopyableFootprints(&desc, 0, 1, 0, &footprint, &rows, &rowSize, &total);
			if (!s.allocator && (FAILED(g_device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&s.allocator))) ||
			                        FAILED(g_device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, s.allocator, nullptr, IID_PPV_ARGS(&s.list))) ||
			                        FAILED(s.list->Close())))
				return false;
			if (s.size < total)
			{
				if (s.buffer)
					s.buffer->Release();
				s.buffer = nullptr;
				D3D12_HEAP_PROPERTIES heap{};
				heap.Type = D3D12_HEAP_TYPE_READBACK;
				D3D12_RESOURCE_DESC buffer{};
				buffer.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
				buffer.Width = total;
				buffer.Height = buffer.DepthOrArraySize = buffer.MipLevels = 1;
				buffer.SampleDesc.Count = 1;
				buffer.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
				if (FAILED(g_device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &buffer, D3D12_RESOURCE_STATE_COPY_DEST, nullptr, IID_PPV_ARGS(&s.buffer))))
					return false;
				s.size = total;
			}
			s.footprint = footprint;
			s.width = static_cast<UINT>(desc.Width), s.height = desc.Height, s.format = desc.Format;
			return true;
		}
	}

	bool Start(const std::wstring& path, double seconds, double fps, int scale, std::string& error)
	{
		std::lock_guard lock(g_mutex);
		if (g_busy)
		{
			error = "a recording is under way";
			return false;
		}
		g_path = path;
		g_seconds = std::clamp(seconds, 0.1, 600.0);
		g_interval = fps > 0 ? 1.0 / fps : 0;
		g_scale = std::clamp(scale, 1, 4);
		g_lastTaken = -1;
		g_taken = g_dropped = 0;
		g_written = 0;
		g_presents.clear();
		QueryPerformanceFrequency(&g_frequency);
		QueryPerformanceCounter(&g_start);
		g_busy = true;
		g_taking = true;
		std::thread([] {
			// (the first frame makes the device objects: the worker waits for them)
			while (g_taking && !g_fence)
				Sleep(5);
			if (g_fence)
				Write();
			else
				g_busy = false;
		}).detach();
		return true;
	}

	bool Busy()
	{
		return g_busy;
	}

	void SetFrameInfo(uint32_t a, uint32_t b, uint32_t c, uint32_t d)
	{
		g_info[0] = a, g_info[1] = b, g_info[2] = c, g_info[3] = d;
	}

	void OnPresent(IDXGISwapChain3* swapChain, ID3D12CommandQueue* queue)
	{
		if (!g_taking)
			return;
		std::lock_guard lock(g_mutex);
		const double now = Now();
		if (now > g_seconds)
		{
			g_taking = false;
			return;
		}
		g_presents.push_back(now);
		if (g_interval > 0 && g_lastTaken >= 0 && now - g_lastTaken < g_interval - 0.002)
			return;
		if (!g_device && FAILED(swapChain->GetDevice(IID_PPV_ARGS(&g_device))))
			return;
		if (!g_fence)
		{
			ID3D12Fence* fence = nullptr;
			if (FAILED(g_device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&fence))))
				return;
			g_fence = fence;
		}
		Slot& s = g_slots[g_taken % kSlots];
		if (s.state.load() != 0)
		{
			++g_dropped;
			return;
		}
		ID3D12Resource* backBuffer = nullptr;
		if (FAILED(swapChain->GetBuffer(swapChain->GetCurrentBackBufferIndex(), IID_PPV_ARGS(&backBuffer))))
			return;
		if (!Prepare(s, backBuffer->GetDesc()))
		{
			backBuffer->Release();
			return;
		}
		s.allocator->Reset();
		s.list->Reset(s.allocator, nullptr);
		D3D12_RESOURCE_BARRIER barrier{};
		barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
		barrier.Transition.pResource = backBuffer;
		barrier.Transition.StateBefore = D3D12_RESOURCE_STATE_PRESENT;
		barrier.Transition.StateAfter = D3D12_RESOURCE_STATE_COPY_SOURCE;
		s.list->ResourceBarrier(1, &barrier);
		D3D12_TEXTURE_COPY_LOCATION to{s.buffer, D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT};
		to.PlacedFootprint = s.footprint;
		D3D12_TEXTURE_COPY_LOCATION from{backBuffer, D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX};
		from.SubresourceIndex = 0;
		s.list->CopyTextureRegion(&to, 0, 0, 0, &from, nullptr);
		std::swap(barrier.Transition.StateBefore, barrier.Transition.StateAfter);
		s.list->ResourceBarrier(1, &barrier);
		s.list->Close();
		ID3D12CommandList* lists[] = {s.list};
		queue->ExecuteCommandLists(1, lists);
		queue->Signal(g_fence, ++g_fenceValue);
		backBuffer->Release();
		s.fence = g_fenceValue;
		s.time = now;
		for (int i = 0; i < 4; ++i)
			s.info[i] = g_info[i];
		s.state = 1;
		++g_taken;
		g_lastTaken = now;
	}
}
