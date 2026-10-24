#include "bridge.hpp"

#include <Windows.h>
#include <d3d12.h>

#include <algorithm>
#include <unordered_map>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <deque>
#include <filesystem>
#include <format>
#include <fstream>
#include <mutex>
#include <set>
#include <string>
#include <thread>
#include <vector>

#include "../config.hpp"
#include "../game/natives.hpp"
#include "../log.hpp"
#include "../paths.hpp"
#include "../state.hpp"
#include "../ui/dx12_hook.hpp"
#include "../ui/recorder.hpp"
#include "GTAVBridgeProtocol.h"
#include "composite.hpp"
#include "gamelights.hpp"
#include "interiorsets.hpp"
#include "streamline.hpp"

namespace loader::graphics::bridge
{
	namespace
	{
		namespace natives = game::natives;

		constexpr uint64_t kGetClockHours = 0x25223CA6B4D20B7F,
		                   kGetClockMinutes = 0x13D2B8ADD79640F2, kGetClockSeconds = 0x494E97C2EF27C470,
		                   kGetCurrWeatherState = 0xF3BBE884A14BB413, kIsIplActive = 0x88A741E44A2B3495,
		                   kGetWindSpeed = 0xA8CF1CC0AFCD3F12, kGetWindDirection = 0x1F400FEF721170DA,
		                   kGetClockYear = 0x961777E64BDAF717, kGetClockMonth = 0xBBC72712E80257A1, kGetClockDayOfMonth = 0x3D10BC92A4DB1D35,
		                   kPlayerPedId = 0xD80958FC74E988A6, kGetPedNearbyPeds = 0x23F8F5FC7E8C4A6B,
		                   kGetPedNearbyVehicles = 0xCFF869CBFA210D82, kGetVehiclePedIsIn = 0x9A9112A0FE9A4713,
		                   kIsPedInAnyVehicle = 0x997ABD671D25CA0B, kDoesEntityExist = 0x7239B21A38F536BA,
		                   kIsEntityVisible = 0x47D6F43D77935C75, kGetEntityMatrix = 0xECB2FC7235A7D137,
		                   kGetEntityModel = 0x9F47B058362C84B5, kGetModelDimensions = 0x03E8D3D5F549087A,
		                   kGetInteriorFromPrimaryView = 0xE7D267EC6CA966C3, kGetRoomKeyForGameViewport = 0xA6575914D2A0B450,
		                   kGetVehicleLightsState = 0xB91B4C20085BD12F, kGetFinalRenderedCamCoord = 0xA200EB1EE790F448;

		// Game thread.
		HANDLE g_mapping = nullptr;
		GTAVBridge::Shared* g_shared = nullptr;
		HANDLE g_process = nullptr, g_job = nullptr;
		bool g_failed = false;
		std::vector<std::string> g_groups; // map data group names (the pack's groups.txt)
		uint32_t g_tick = 0;
		std::filesystem::file_time_type g_consoleStamp{};
		std::string g_presetSent; // the preset the renderer has
		uint32_t g_key = VK_F11;

		// Render threads: the picture shown, the size DLSS gives, the renderer's texture and fence on the game's device.
		std::atomic_bool g_showing = false;
		std::atomic_bool g_restart = false; // the hotkey while the renderer is not running
		std::atomic<uint32_t> g_width = 0, g_height = 0;
		std::mutex g_renderMutex;
		ID3D12Resource* g_texture[GTAVBridge::kTextures] = {}; // (the renderer's frame for game frame s is in g_texture[s % kTextures])
		ID3D12Fence* g_fence = nullptr;
		ID3D12Fence* g_consumed = nullptr; // (signalled with a game frame once it was presented: its texture is free again)
		std::vector<uint64_t> g_composed;  // the frames composed and not yet presented, oldest first
		uint64_t g_consumedValue = 0;
		uint32_t g_openedSequence = 0;
		std::vector<std::pair<std::chrono::steady_clock::time_point, IUnknown*>> g_retired; // released a while later
		bool g_warnedSize = false;

		// Timing (the pace line every 10 s): for each frame composed, when the game's queue came to its wait for the
		// renderer's frame (a fence of ours signalled just before it) and when that frame was done, both on the GPU as a
		// waiting thread sees them, and when the frame's camera came.
		struct Timing
		{
			uint64_t arrive, value;
			int64_t camera;      // (QueryPerformanceCounter)
			ID3D12Fence* frame;  // (a reference: the renderer may make its fence anew)
		};
		ID3D12Fence* g_arrive = nullptr;
		uint64_t g_arriveValue = 0;
		std::mutex g_timingMutex;
		std::condition_variable g_timingWake;
		std::deque<Timing> g_timing;
		int64_t g_cameraTime[GTAVBridge::kViews] = {}; // (by sequence % kViews)
		struct Totals
		{
			double waited = 0, latency = 0, toWait = 0;
			uint32_t frames = 0, stalled = 0;
		};
		Totals g_totals; // (g_timingMutex)

		int64_t Now()
		{
			LARGE_INTEGER t;
			QueryPerformanceCounter(&t);
			return t.QuadPart;
		}

		double Milliseconds(int64_t ticks)
		{
			static const double perMs = [] {
				LARGE_INTEGER f;
				QueryPerformanceFrequency(&f);
				return static_cast<double>(f.QuadPart) / 1000.0;
			}();
			return static_cast<double>(ticks) / perMs;
		}

		void TimingThread()
		{
			const HANDLE event = CreateEventW(nullptr, FALSE, FALSE, nullptr);
			for (;;)
			{
				Timing t;
				{
					std::unique_lock lock(g_timingMutex);
					g_timingWake.wait(lock, [] { return !g_timing.empty(); });
					t = g_timing.front();
					g_timing.pop_front();
				}
				if (SUCCEEDED(g_arrive->SetEventOnCompletion(t.arrive, event)) && WaitForSingleObject(event, 2000) == WAIT_OBJECT_0)
				{
					const int64_t arrived = Now();
					if (SUCCEEDED(t.frame->SetEventOnCompletion(t.value, event)) && WaitForSingleObject(event, 2000) == WAIT_OBJECT_0)
					{
						const int64_t done = Now();
						std::lock_guard lock(g_timingMutex);
						const double waited = Milliseconds(done - arrived);
						g_totals.waited += waited;
						g_totals.stalled += waited > 1.0;
						if (t.camera)
						{
							g_totals.latency += Milliseconds(done - t.camera);
							g_totals.toWait += Milliseconds(arrived - t.camera);
						}
						++g_totals.frames;
					}
				}
				t.frame->Release();
			}
		}

		std::filesystem::path Path(const std::string& utf8)
		{
			return std::filesystem::path(reinterpret_cast<const char8_t*>(utf8.c_str()));
		}

		uint32_t KeyFromName(const std::string& name)
		{
			if (name.size() >= 2 && (name[0] == 'F' || name[0] == 'f'))
				if (const int n = std::atoi(name.c_str() + 1); n >= 1 && n <= 24)
					return VK_F1 + n - 1;
			return name.size() == 1 ? static_cast<uint32_t>(std::toupper(static_cast<unsigned char>(name[0]))) : VK_F11;
		}

		std::wstring Named(const wchar_t* prefix)
		{
			return prefix + std::to_wstring(GetCurrentProcessId());
		}

		// The game's own settings while UE5 draws the world, written to its settings.xml before the game reads it (when
		// it starts) and the person's own put back once the game renders (ModLoader/ue5_settings.txt keeps them
		// meanwhile, should the game stop before):
		// - its render resolution (config ue5GameResolution): the game asks Streamline nothing, it takes
		//   <dlssQuality value="..."/> (0 Performance, 1 Balanced, 2 Quality, 3 DLAA, measured);
		// - its world's detail (config ue5GameDetail "lean"): of the game's picture only the peds and vehicles are
		//   shown, so what only draws the world it need not (grass, water, tessellation, volumetric fog, ray traced
		//   trees, the farther levels of detail, its ray tracing: its shadow maps shade the peds and vehicles as well;
		//   in a storm it cost the shared GPU some 4 ms a frame, research §60.6); the peds' and vehicles' own (their
		//   shadows, reflections) stay, and what acts on the whole picture after the renderer's goes in (depth of
		//   field, motion blur).
		std::filesystem::path g_settingsFile;
		bool g_settingsRestored = true;

		std::filesystem::path SettingsFile()
		{
			wchar_t documents[MAX_PATH]{};
			DWORD size = sizeof(documents);
			// (the registry, not the shell: this runs while the process loads)
			if (RegGetValueW(HKEY_CURRENT_USER, L"Software\\Microsoft\\Windows\\CurrentVersion\\Explorer\\Shell Folders", L"Personal", RRF_RT_REG_SZ,
			        nullptr, documents, &size) != ERROR_SUCCESS)
				return {};
			return std::filesystem::path(documents) / L"Rockstar Games" / L"GTAV Enhanced" / L"settings.xml";
		}

		using Settings = std::vector<std::pair<std::string, std::string>>;

		// The settings file with each of `values` set (<name value="..." />); the values they had (those it has).
		Settings SetSettings(const std::filesystem::path& file, const Settings& values)
		{
			std::ifstream in(file, std::ios::binary);
			std::string text((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
			in.close();
			Settings old;
			bool changed = false;
			for (const auto& [name, value] : values)
			{
				const std::string key = "<" + name + " value=\"";
				const size_t at = text.find(key);
				if (at == std::string::npos)
					continue;
				const size_t from = at + key.size(), to = text.find('"', from);
				if (to == std::string::npos)
					continue;
				old.emplace_back(name, text.substr(from, to - from));
				if (old.back().second != value)
				{
					text.replace(from, to - from, value);
					changed = true;
				}
			}
			if (changed && !text.empty())
				std::ofstream(file, std::ios::binary | std::ios::trunc).write(text.data(), static_cast<std::streamsize>(text.size()));
			return old;
		}

		// ModLoader/ue5_settings.txt: name=value a line.
		Settings ReadSaved(const std::filesystem::path& saved)
		{
			Settings out;
			std::ifstream in(saved);
			for (std::string line; std::getline(in, line);)
				if (const size_t eq = line.find('='); eq != std::string::npos)
					out.emplace_back(line.substr(0, eq), line.substr(eq + 1));
			return out;
		}

		void PrepareGameSettings()
		{
			const auto& c = config::Get();
			const auto root = paths::Get().root;
			g_settingsFile = SettingsFile();
			if (g_settingsFile.empty())
				return;
			// (a run that stopped before putting them back; ue5_dlssquality.txt: an older loader's)
			std::error_code ec;
			if (std::ifstream old(root / L"ue5_dlssquality.txt"); old)
			{
				int value = -1;
				if (old >> value; value >= 0)
					SetSettings(g_settingsFile, {{"dlssQuality", std::to_string(value)}});
				old.close();
				std::filesystem::remove(root / L"ue5_dlssquality.txt", ec);
			}
			const auto saved = root / L"ue5_settings.txt";
			if (const Settings back = ReadSaved(saved); !back.empty())
				SetSettings(g_settingsFile, back);
			std::filesystem::remove(saved, ec);
			if (!c.ue5Renderer)
				return;
			Settings wanted;
			const std::string& r = c.ue5GameResolution;
			if (r == "performance" || r == "balanced" || r == "quality")
				wanted.emplace_back("dlssQuality", r == "performance" ? "0" : r == "balanced" ? "1" : "2");
			static constexpr std::pair<const char*, const char*> kLean[] = {{"GrassQuality", "0"}, {"WaterQuality", "0"}, {"Tessellation", "0"},
			    {"LodScale", "0.600000"}, {"MaxLodScale", "0.000000"}, {"Lighting_FogVolumes", "false"}, {"Shadow_LongShadows", "false"},
			    {"Raytracing_TreeBvhEnabled", "false"}, {"Raytracing_GrassBvhEnabled", "false"}, {"Raytracing_Enabled", "false"},
			    {"RTShadows_Enabled", "false"}};
			if (c.ue5GameDetail == "lean")
				for (const auto& [name, value] : kLean)
					wanted.emplace_back(name, value);
			if (wanted.empty())
				return;
			const Settings original = SetSettings(g_settingsFile, wanted);
			std::ofstream out(saved);
			std::string list;
			for (const auto& [name, value] : original)
			{
				out << name << '=' << value << '\n';
				list += std::format(" {}", name);
			}
			g_settingsRestored = false;
			log::Info("ue5 bridge: the game's own picture with{} changed (put back once it renders)", list);
		}

		void RestoreGameSettings()
		{
			if (g_settingsRestored)
				return;
			g_settingsRestored = true;
			const auto saved = paths::Get().root / L"ue5_settings.txt";
			if (const Settings back = ReadSaved(saved); !back.empty())
				SetSettings(g_settingsFile, back);
			std::error_code ec;
			std::filesystem::remove(saved, ec);
		}

		void Retire(IUnknown* object)
		{
			if (object)
				g_retired.emplace_back(std::chrono::steady_clock::now(), object);
		}

		// The renderer's texture and fence, opened (again) on the game's device after the renderer made them anew.
		bool Open(ID3D12Device* device)
		{
			const uint32_t sequence = g_shared->textureSequence;
			if (sequence == g_openedSequence && g_texture[GTAVBridge::kTextures - 1] && g_fence && g_consumed)
				return true;
			for (ID3D12Resource*& t : g_texture)
				Retire(t), t = nullptr;
			Retire(g_fence);
			Retire(g_consumed);
			g_fence = g_consumed = nullptr;
			g_composed.clear();
			g_consumedValue = 0;
			g_openedSequence = sequence;
			if (!sequence)
				return false;
			ID3D12Device1* device1 = nullptr;
			if (FAILED(device->QueryInterface(IID_PPV_ARGS(&device1))))
				return false;
			bool ok = true;
			for (const auto& [name, out] : {std::pair{GTAVBridge::kFenceName, &g_fence}, std::pair{GTAVBridge::kConsumedName, &g_consumed}})
			{
				HANDLE fence = nullptr;
				ok = ok && SUCCEEDED(device1->OpenSharedHandleByName(Named(name).c_str(), GENERIC_ALL, &fence)) &&
				     SUCCEEDED(device->OpenSharedHandle(fence, IID_PPV_ARGS(out)));
				if (fence)
					CloseHandle(fence);
			}
			for (uint32_t i = 0; i < GTAVBridge::kTextures && ok; ++i)
			{
				HANDLE texture = nullptr;
				ok = SUCCEEDED(device1->OpenSharedHandleByName((Named(GTAVBridge::kTextureName) + L"-" + std::to_wstring(i)).c_str(), GENERIC_ALL, &texture)) &&
				     SUCCEEDED(device->OpenSharedHandle(texture, IID_PPV_ARGS(&g_texture[i])));
				if (texture)
					CloseHandle(texture);
			}
			device1->Release();
			log::Info("ue5 bridge: renderer texture {} ({}x{})", ok ? "opened" : "could not be opened", g_shared->textureWidth, g_shared->textureHeight);
			return ok;
		}

		// Research: the depth and stencil the game gave DLSS, read back once on request (bridge::DumpDepth).
		std::mutex g_dumpMutex;
		std::wstring g_dumpPath;
		ID3D12Resource* g_dumpBuffer = nullptr;

		void DumpDepth(const streamline::DlssFrame& frame)
		{
			std::wstring path;
			{
				std::lock_guard lock(g_dumpMutex);
				if (g_dumpPath.empty() || !frame.depth)
					return;
				path = std::move(g_dumpPath);
				g_dumpPath.clear();
			}
			auto* depth = static_cast<ID3D12Resource*>(frame.depth);
			ID3D12Device* device = nullptr;
			if (FAILED(depth->GetDevice(IID_PPV_ARGS(&device))))
				return;
			const D3D12_RESOURCE_DESC desc = depth->GetDesc();
			D3D12_PLACED_SUBRESOURCE_FOOTPRINT footprints[2]{};
			UINT rows[2]{};
			UINT64 rowBytes[2]{}, total = 0;
			device->GetCopyableFootprints(&desc, 0, 2, 0, footprints, rows, rowBytes, &total);
			D3D12_HEAP_PROPERTIES heap{};
			heap.Type = D3D12_HEAP_TYPE_READBACK;
			D3D12_RESOURCE_DESC buffer{};
			buffer.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
			buffer.Width = total;
			buffer.Height = buffer.DepthOrArraySize = buffer.MipLevels = 1;
			buffer.SampleDesc.Count = 1;
			buffer.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
			ID3D12Resource* readback = nullptr;
			const bool made = SUCCEEDED(device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &buffer, D3D12_RESOURCE_STATE_COPY_DEST, nullptr, IID_PPV_ARGS(&readback)));
			device->Release();
			if (!made)
				return;
			auto* list = static_cast<ID3D12GraphicsCommandList*>(frame.commandList);
			D3D12_RESOURCE_BARRIER barrier{};
			barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
			barrier.Transition.pResource = depth;
			barrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
			barrier.Transition.StateBefore = static_cast<D3D12_RESOURCE_STATES>(frame.depthState);
			barrier.Transition.StateAfter = D3D12_RESOURCE_STATE_COPY_SOURCE;
			list->ResourceBarrier(1, &barrier);
			for (UINT plane = 0; plane < 2; ++plane)
			{
				D3D12_TEXTURE_COPY_LOCATION to{readback, D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT};
				to.PlacedFootprint = footprints[plane];
				D3D12_TEXTURE_COPY_LOCATION from{depth, D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX};
				from.SubresourceIndex = plane;
				list->CopyTextureRegion(&to, 0, 0, 0, &from, nullptr);
			}
			std::swap(barrier.Transition.StateBefore, barrier.Transition.StateAfter);
			list->ResourceBarrier(1, &barrier);
			// (the game runs the list soon: read it a moment later)
			std::thread([readback, footprints0 = footprints[0], footprints1 = footprints[1], rows0 = rows[0], rows1 = rows[1], path,
			                width = frame.renderWidth, height = frame.renderHeight] {
				Sleep(2000);
				void* data = nullptr;
				if (SUCCEEDED(readback->Map(0, nullptr, &data)))
				{
					std::ofstream out(std::filesystem::path(path), std::ios::binary);
					const uint32_t header[8] = {footprints0.Footprint.Width, rows0, footprints0.Footprint.RowPitch, static_cast<uint32_t>(footprints0.Offset),
					    footprints1.Footprint.Width, rows1, footprints1.Footprint.RowPitch, static_cast<uint32_t>(footprints1.Offset)};
					out.write(reinterpret_cast<const char*>(header), sizeof(header));
					const uint32_t used[2] = {width, height};
					out.write(reinterpret_cast<const char*>(used), sizeof(used));
					const size_t size = footprints1.Offset + size_t{footprints1.Footprint.RowPitch} * rows1;
					out.write(static_cast<const char*>(data), static_cast<std::streamsize>(size));
					readback->Unmap(0, nullptr);
					log::Info("ue5 bridge: depth and stencil written ({}x{}, in use {}x{})", footprints0.Footprint.Width, rows0, width, height);
				}
				readback->Release();
			}).detach();
		}

		// Research: the game's own scene colour going into DLSS (linear HDR, its render resolution) to a file (header: width,
		// rows, row pitch, DXGI format, the size in use; then the rows), for measuring its lights against the renderer's.
		std::wstring g_colorPath;

		void DumpColor(const streamline::DlssFrame& frame)
		{
			std::wstring path;
			{
				std::lock_guard lock(g_dumpMutex);
				if (g_colorPath.empty() || !frame.input)
					return;
				path = std::move(g_colorPath);
				g_colorPath.clear();
			}
			auto* input = static_cast<ID3D12Resource*>(frame.input);
			ID3D12Device* device = nullptr;
			if (FAILED(input->GetDevice(IID_PPV_ARGS(&device))))
				return;
			const D3D12_RESOURCE_DESC desc = input->GetDesc();
			D3D12_PLACED_SUBRESOURCE_FOOTPRINT footprint{};
			UINT rows = 0;
			UINT64 rowBytes = 0, total = 0;
			device->GetCopyableFootprints(&desc, 0, 1, 0, &footprint, &rows, &rowBytes, &total);
			D3D12_HEAP_PROPERTIES heap{};
			heap.Type = D3D12_HEAP_TYPE_READBACK;
			D3D12_RESOURCE_DESC buffer{};
			buffer.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
			buffer.Width = total;
			buffer.Height = buffer.DepthOrArraySize = buffer.MipLevels = 1;
			buffer.SampleDesc.Count = 1;
			buffer.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
			ID3D12Resource* readback = nullptr;
			const bool made = SUCCEEDED(device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &buffer, D3D12_RESOURCE_STATE_COPY_DEST, nullptr, IID_PPV_ARGS(&readback)));
			device->Release();
			if (!made)
				return;
			auto* list = static_cast<ID3D12GraphicsCommandList*>(frame.commandList);
			D3D12_RESOURCE_BARRIER barrier{};
			barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
			barrier.Transition.pResource = input;
			barrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
			barrier.Transition.StateBefore = static_cast<D3D12_RESOURCE_STATES>(frame.inputState);
			barrier.Transition.StateAfter = D3D12_RESOURCE_STATE_COPY_SOURCE;
			list->ResourceBarrier(1, &barrier);
			D3D12_TEXTURE_COPY_LOCATION to{readback, D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT};
			to.PlacedFootprint = footprint;
			D3D12_TEXTURE_COPY_LOCATION from{input, D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX};
			from.SubresourceIndex = 0;
			list->CopyTextureRegion(&to, 0, 0, 0, &from, nullptr);
			std::swap(barrier.Transition.StateBefore, barrier.Transition.StateAfter);
			list->ResourceBarrier(1, &barrier);
			std::thread([readback, footprint, rows, path, width = frame.renderWidth, height = frame.renderHeight] {
				Sleep(2000);
				void* data = nullptr;
				if (SUCCEEDED(readback->Map(0, nullptr, &data)))
				{
					std::ofstream out(std::filesystem::path(path), std::ios::binary);
					const uint32_t header[6] = {footprint.Footprint.Width, rows, footprint.Footprint.RowPitch, static_cast<uint32_t>(footprint.Footprint.Format),
					    width, height};
					out.write(reinterpret_cast<const char*>(header), sizeof(header));
					out.write(static_cast<const char*>(data), static_cast<std::streamsize>(size_t{footprint.Footprint.RowPitch} * rows));
					readback->Unmap(0, nullptr);
					log::Info("ue5 bridge: the game's scene colour written ({}x{}, format {}, in use {}x{})", footprint.Footprint.Width, rows,
					    static_cast<int>(footprint.Footprint.Format), width, height);
				}
				readback->Release();
			}).detach();
		}

		// The game's frame this render thread records (its camera's sequence number): the renderer's frame to wait for;
		// and its camera (the depth's projection).
		thread_local uint32_t t_sequence = 0;
		thread_local streamline::Camera t_camera;
		uint64_t g_lastValue = 0;      // the renderer's newest frame seen, and when it last moved on
		ULONGLONG g_lastProgress = 0;

		// Right after the game's DLSS pass: the renderer's frame of this camera over DLSS's output, once the game's
		// queue has waited for it (frame for frame, so that the game's peds and vehicles sit on the renderer's
		// picture exactly; while the renderer stalls, its newest frame instead). The frame's texture is the renderer's
		// again once the frame was presented (OnPresent).
		void AfterDlss(const streamline::DlssFrame& frame)
		{
			DumpDepth(frame);
			DumpColor(frame);
			void* commandList = frame.commandList;
			const uint32_t outputState = frame.outputState;
			auto* target = static_cast<ID3D12Resource*>(frame.output);
			const D3D12_RESOURCE_DESC desc = target->GetDesc();
			g_width = static_cast<uint32_t>(desc.Width);
			g_height = desc.Height;
			if (!g_showing || !g_shared || g_shared->rendererState != 1 || !g_shared->frameValue)
				return;
			ID3D12CommandQueue* queue = ui::dx12::PresentQueue();
			if (!queue)
				return;
			std::lock_guard lock(g_renderMutex);
			const auto now = std::chrono::steady_clock::now();
			std::erase_if(g_retired, [&](auto& r) {
				if (now - r.first < std::chrono::seconds(2))
					return false;
				r.second->Release();
				return true;
			});
			ID3D12Device* device = nullptr;
			if (FAILED(target->GetDevice(IID_PPV_ARGS(&device))))
				return;
			const bool opened = Open(device);
			device->Release();
			if (!opened)
				return;
			const D3D12_RESOURCE_DESC source = g_texture[0]->GetDesc();
			if (source.Width != desc.Width || source.Height != desc.Height || source.Format != desc.Format)
			{
				if (!g_warnedSize)
					log::Info("ue5 bridge: renderer frame {}x{} format {}, the game's {}x{} format {}: waiting for it to match", source.Width, source.Height,
					    static_cast<int>(source.Format), desc.Width, desc.Height, static_cast<int>(desc.Format));
				g_warnedSize = true;
				return;
			}
			g_warnedSize = false;
			// (the game's queue runs this list later: the wait goes in first)
			const uint64_t newest = g_shared->frameValue;
			const ULONGLONG tick = GetTickCount64();
			if (newest != g_lastValue)
				g_lastValue = newest, g_lastProgress = tick;
			const bool stalled = tick - g_lastProgress > 500;
			const uint64_t value = stalled || !t_sequence ? newest : t_sequence;
			// (timing: our fence just before the wait)
			if (!g_arrive)
			{
				if (ID3D12Device* d = nullptr; SUCCEEDED(target->GetDevice(IID_PPV_ARGS(&d))))
				{
					if (SUCCEEDED(d->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&g_arrive))))
						std::thread(TimingThread).detach();
					d->Release();
				}
			}
			if (g_arrive && SUCCEEDED(queue->Signal(g_arrive, ++g_arriveValue)))
			{
				g_fence->AddRef();
				std::lock_guard timingLock(g_timingMutex);
				g_timing.push_back({g_arriveValue, value, g_cameraTime[value % GTAVBridge::kViews], g_fence});
				while (g_timing.size() > 16)
				{
					g_timing.front().frame->Release();
					g_timing.pop_front();
				}
				g_timingWake.notify_one();
			}
			queue->Wait(g_fence, value);
			ID3D12Resource* picture = g_texture[value % GTAVBridge::kTextures];
			ui::recorder::SetFrameInfo(t_sequence, static_cast<uint32_t>(value), static_cast<uint32_t>(newest),
			    (stalled ? 1u : 0u) | static_cast<uint32_t>(composite::Flash() * 100.0f) << 8);
			g_composed.push_back(value);
			if (g_composed.size() > 8)
				g_composed.erase(g_composed.begin());
			// (the game's peds and vehicles kept, where the renderer's world is not in front of them; a plain copy when
			// that cannot be done)
			composite::Settings settings;
			settings.match = std::clamp(config::Get().ue5ExposureMatch, 0.0f, 1.0f);
			settings.grade = std::clamp(config::Get().ue5GradeMatch, 0.0f, 1.0f);
			// (part of following the game's grading, indoors: outdoors the renderer's own ground - its grass, their
			// shadows - is wider in brightness than the game's lean one, and narrowed to it the whole scene went flat)
			settings.contrast = g_shared && g_shared->interior != 0 ? settings.grade : 0.0f;
			// (the game's depth to metres: its viewToClip for row vectors gives depth = (z m10 + m14) / (z m11), so
			// z = m14 / (depth m11 - m10); for column vectors m11 and m14 trade places: w comes from z by the -1)
			const float* m = t_camera.viewToClip;
			const bool rows = std::abs(m[11]) > std::abs(m[14]);
			settings.depthNumerator = rows ? m[14] : m[11];
			settings.depthScale = rows ? m[11] : m[14];
			settings.depthOffset = m[10];
			if (composite::Record(frame, picture, settings))
				return;
			auto* list = static_cast<ID3D12GraphicsCommandList*>(commandList);
			D3D12_RESOURCE_BARRIER barrier{};
			barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
			barrier.Transition.pResource = target;
			barrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
			barrier.Transition.StateBefore = static_cast<D3D12_RESOURCE_STATES>(outputState);
			barrier.Transition.StateAfter = D3D12_RESOURCE_STATE_COPY_DEST;
			list->ResourceBarrier(1, &barrier);
			list->CopyResource(target, picture);
			std::swap(barrier.Transition.StateBefore, barrier.Transition.StateAfter);
			list->ResourceBarrier(1, &barrier);
		}

		bool CreateShared()
		{
			const std::wstring name = Named(GTAVBridge::kMemoryName);
			g_mapping = CreateFileMappingW(INVALID_HANDLE_VALUE, nullptr, PAGE_READWRITE, 0, sizeof(GTAVBridge::Shared), name.c_str());
			if (!g_mapping)
				return false;
			g_shared = static_cast<GTAVBridge::Shared*>(MapViewOfFile(g_mapping, FILE_MAP_ALL_ACCESS, 0, 0, sizeof(GTAVBridge::Shared)));
			if (!g_shared)
				return false;
			std::memset(g_shared, 0, sizeof(GTAVBridge::Shared));
			g_shared->magic = GTAVBridge::kMagic;
			g_shared->version = GTAVBridge::kVersion;
			gamelights::SetTarget(g_shared);
			return true;
		}

		bool Launch()
		{
			const auto& c = config::Get();
			if (c.ue5Editor.empty() || c.ue5Project.empty() || c.ue5Pack.empty())
			{
				log::Warn("ue5 bridge: loader.json needs ue5Editor, ue5Project and ue5Pack");
				return false;
			}
			std::error_code ec;
			for (const auto& path : {c.ue5Editor, c.ue5Project, c.ue5Pack})
				if (!std::filesystem::exists(Path(path), ec))
				{
					log::Warn("ue5 bridge: {} not found", path);
					return false;
				}
			std::ifstream groups(Path(c.ue5Pack) / "groups.txt");
			std::set<std::string> unique;
			g_groups.clear();
			for (std::string line; std::getline(groups, line);)
				if (!line.empty() && unique.insert(line).second)
					g_groups.push_back(line);
			const auto log = (paths::Get().root / L"ue5renderer.log").wstring();
			std::wstring command = std::format(L"\"{}\" \"{}\" /Engine/Maps/Entry -game -RenderOffscreen -nosound -unattended -nosplash -gtavsky "
			                                   L"-gtavpack=\"{}\" -gtavbridge={} -ResX={} -ResY={} -log -abslog=\"{}\"",
			    Path(c.ue5Editor).wstring(), Path(c.ue5Project).wstring(), Path(c.ue5Pack).wstring(),
			    GetCurrentProcessId(), g_width.load(), g_height.load(), log);
			// (checking: more of the renderer's switches, from ModLoader/ue5args.txt)
			if (std::ifstream extra(paths::Get().root / L"ue5args.txt"); extra)
				for (std::string word; extra >> word;)
					command += L" " + std::wstring(word.begin(), word.end());
			STARTUPINFOW si{sizeof(si)};
			PROCESS_INFORMATION pi{};
			if (!CreateProcessW(nullptr, command.data(), nullptr, nullptr, FALSE, CREATE_NO_WINDOW | CREATE_SUSPENDED | BELOW_NORMAL_PRIORITY_CLASS, nullptr,
			        nullptr, &si, &pi))
			{
				log::Error("ue5 bridge: could not start the renderer ({})", GetLastError());
				return false;
			}
			// (it goes when the game goes)
			g_job = CreateJobObjectW(nullptr, nullptr);
			JOBOBJECT_EXTENDED_LIMIT_INFORMATION limits{};
			limits.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
			if (g_job)
			{
				SetInformationJobObject(g_job, JobObjectExtendedLimitInformation, &limits, sizeof(limits));
				AssignProcessToJobObject(g_job, pi.hProcess);
			}
			ResumeThread(pi.hThread);
			CloseHandle(pi.hThread);
			g_process = pi.hProcess;
			log::Info("ue5 bridge: renderer started ({}x{}, {} map data groups)", g_width.load(), g_height.load(), g_groups.size());
			return true;
		}

		// A quality preset as the renderer's console commands: the share of the output resolution it renders (the
		// engine's upscaler does the rest), the engine's scalability groups (0 low .. 4 cinematic), how far the map's
		// distances reach (gtav.LodScale) and how many of the map's lights near the camera are real lights.
		std::string PresetCommands(const std::string& preset)
		{
			// (MegaLights from high up: every light of the map within reach lights the scene, at a near constant cost;
			// without it a few dozen of the most important)
			struct Level
			{
				const char* name;
				int resolution, scalability;
				float distance;
				int lights, lightRadius;
				bool megaLights, lightShadows;
			};
			static constexpr Level kLevels[] = {{"low", 50, 0, 0.8f, 32, 150, false, false}, {"medium", 67, 1, 1.0f, 64, 200, false, false},
			    {"high", 80, 2, 1.25f, 2048, 400, true, false}, {"ultra", 100, 3, 1.5f, 4096, 600, true, false},
			    {"cinematic", 100, 4, 2.0f, 8192, 800, true, true}};
			const Level* l = &kLevels[2];
			for (const Level& k : kLevels)
				if (preset == k.name)
					l = &k;
			std::string out = std::format("r.ScreenPercentage {}\n", l->resolution);
			for (const char* group : {"ViewDistance", "Shadow", "GlobalIllumination", "Reflection", "PostProcess", "Texture", "Effects", "Foliage", "Shading"})
				out += std::format("sg.{}Quality {}\n", group, l->scalability);
			out += std::format("gtav.LodScale {}\ngtav.Lights {}\ngtav.LightRadius {}\nr.MegaLights.EnableForProject {}\ngtav.LightShadows {}\n", l->distance,
			    l->lights, l->lightRadius, l->megaLights ? 1 : 0, l->lightShadows ? 1 : 0);
			// (with MegaLights the sun and the moon too, their shadows ray traced: a shadow map of the city for them was
			// thousands of draws a frame and the longest part of the renderer's frame; measured 44 -> 60 fps by night)
			// (without it the moon casts none: by night its shadow map costs as much as the sun's, for shadows street lights
			// wash out)
			out += std::format("r.Forward.LightBuffer.Mode {0}\nr.MegaLights.DirectionalLights {0}\ngtav.MoonShadows {0}\n", l->megaLights ? 1 : 0);
			return out;
		}

		// The peds and vehicles near the player (the game's lists of them around the player's ped; a ped in a vehicle is
		// the vehicle's), their place, axes and model bounds: the renderer casts their shadows.
		void WriteProxies(GTAVBridge::Shared& s)
		{
			static std::unordered_map<uint32_t, std::pair<natives::ScrVector, natives::ScrVector>> bounds;
			uint32_t n = 0;
			int32_t entities[GTAVBridge::kMaxProxies];
			const auto add = [&](int32_t entity, uint32_t kind) {
				if (n >= GTAVBridge::kMaxProxies || !entity || std::find(entities, entities + n, entity) != entities + n ||
				    !natives::Invoke<int32_t>(kDoesEntityExist, entity) || !natives::Invoke<int32_t>(kIsEntityVisible, entity))
					return;
				const uint32_t model = natives::Invoke<uint32_t>(kGetEntityModel, entity);
				auto it = bounds.find(model);
				if (it == bounds.end())
				{
					natives::ScrVector mn{}, mx{};
					natives::Invoke<void>(kGetModelDimensions, model, &mn, &mx);
					it = bounds.emplace(model, std::make_pair(mn, mx)).first;
				}
				natives::ScrVector forward{}, right{}, up{}, position{};
				natives::Invoke<void>(kGetEntityMatrix, entity, &forward, &right, &up, &position);
				entities[n] = entity;
				GTAVBridge::Proxy& p = s.proxies[n++];
				p.position[0] = position.x, p.position[1] = position.y, p.position[2] = position.z;
				p.forward[0] = forward.x, p.forward[1] = forward.y, p.forward[2] = forward.z;
				p.up[0] = up.x, p.up[1] = up.y, p.up[2] = up.z;
				p.boundsMin[0] = it->second.first.x, p.boundsMin[1] = it->second.first.y, p.boundsMin[2] = it->second.first.z;
				p.boundsMax[0] = it->second.second.x, p.boundsMax[1] = it->second.second.y, p.boundsMax[2] = it->second.second.z;
				p.kind = kind;
				p.lights = 0;
				if (kind == 2)
				{
					int64_t on = 0, full = 0; // (script BOOLs: 8-byte slots)
					natives::Invoke<int32_t>(kGetVehicleLightsState, entity, &on, &full);
					p.lights = (on & 1 ? 1u : 0u) | (full & 1 ? 2u : 0u);
				}
			};
			const int32_t player = natives::Invoke<int32_t>(kPlayerPedId);
			if (const int32_t vehicle = natives::Invoke<int32_t>(kGetVehiclePedIsIn, player, 0))
				add(vehicle, 2);
			else
				add(player, 1);
			int64_t vehicles[1 + 32] = {32};
			const int nv = std::min(natives::Invoke<int32_t>(kGetPedNearbyVehicles, player, vehicles), 32);
			for (int i = 0; i < nv; ++i)
				add(static_cast<int32_t>(vehicles[1 + i]), 2);
			int64_t peds[1 + 32] = {32};
			const int np = std::min(natives::Invoke<int32_t>(kGetPedNearbyPeds, player, peds, -1), 32);
			for (int i = 0; i < np; ++i)
				if (!natives::Invoke<int32_t>(kIsPedInAnyVehicle, static_cast<int32_t>(peds[1 + i]), 0))
					add(static_cast<int32_t>(peds[1 + i]), 1);
			s.proxyCount = n;
		}

		// The game's wet ground (0 dry - 1 soaked: it rises in the rain and dries linearly over about an hour of game time
		// after it, measured), next to its rain level - which GET_RAIN_LEVEL returns: its code, past the obfuscation's
		// jumps, is movss xmm0, [rip + x]; the wetness is 0x24 after that. Null when the code is not as known.
		const float* FindWetness()
		{
			const auto* p = reinterpret_cast<const uint8_t*>(natives::FindHandler(0x96695E368AD855F3));
			for (int hops = 0; p && hops < 4 && p[0] == 0xE9; ++hops)
				p += 5 + *reinterpret_cast<const int32_t*>(p + 1);
			if (!p || p[0] != 0xF3 || p[1] != 0x0F || p[2] != 0x10 || p[3] != 0x05)
				return nullptr;
			const auto* rain = reinterpret_cast<const float*>(p + 8 + *reinterpret_cast<const int32_t*>(p + 4));
			return rain + 9;
		}

		// The view and the world's state, from the game.
		void Write()
		{
			GTAVBridge::Shared& s = *g_shared;
			s.hours = natives::Invoke<int32_t>(kGetClockHours) + natives::Invoke<int32_t>(kGetClockMinutes) / 60.0f +
			          natives::Invoke<int32_t>(kGetClockSeconds) / 3600.0f;
			uint32_t from = 0, to = 0;
			float blend = 0;
			natives::Invoke<void>(kGetCurrWeatherState, &from, &to, &blend);
			s.weatherFrom = from, s.weatherTo = to, s.weatherBlend = blend;
			const auto wind = natives::Invoke<natives::ScrVector>(kGetWindDirection);
			s.wind[0] = wind.x, s.wind[1] = wind.y, s.wind[2] = wind.z;
			s.windSpeed = natives::Invoke<float>(kGetWindSpeed);
			s.year = static_cast<uint32_t>(natives::Invoke<int32_t>(kGetClockYear));
			s.month = static_cast<uint32_t>(natives::Invoke<int32_t>(kGetClockMonth) + 1); // (the game's from 0)
			s.day = static_cast<uint32_t>(natives::Invoke<int32_t>(kGetClockDayOfMonth));
			s.interior = natives::Invoke<int32_t>(kGetInteriorFromPrimaryView);
			s.room = s.interior ? natives::Invoke<uint32_t>(kGetRoomKeyForGameViewport) : 0;
			s.lightning = composite::Flash();
			static const float* wetness = [] {
				const float* w = FindWetness();
				log::Info("ue5 bridge: the game's wet ground {}", w ? std::format("at +{:#x}", reinterpret_cast<uintptr_t>(w) - reinterpret_cast<uintptr_t>(GetModuleHandleW(nullptr))) : std::string("not found (wet by the weather)"));
				return w;
			}();
			s.wetness = wetness && *wetness >= 0.0f && *wetness <= 1.0f ? *wetness : -1.0f;
			if (static int32_t told = -1; s.interior != told)
			{
				told = s.interior;
				log::Info("ue5 bridge: the camera is {} (room {:#x})", s.interior ? std::format("in interior {}", s.interior) : std::string("outside"), s.room);
			}
			WriteProxies(s);
			// The interiors near the camera with their entity sets on (every tenth frame).
			if (g_tick % 10 == 0 && interiorsets::Install())
			{
				const auto camera = natives::Invoke<natives::ScrVector>(kGetFinalRenderedCamCoord);
				const float at[3] = {camera.x, camera.y, camera.z};
				static uint32_t sequence = 0;
				s.interiorSequence = 0;
				MemoryBarrier();
				s.interiorCount = static_cast<uint32_t>(interiorsets::Near(at, 300.0f, s.interiors, GTAVBridge::kMaxInteriors));
				MemoryBarrier();
				s.interiorSequence = ++sequence ? sequence : ++sequence;
			}
			s.width = g_width, s.height = g_height;
			// Pace, every 10 s: the renderer's new frames against the game's.
			static ULONGLONG lastPace = 0;
			static uint64_t lastFrames = 0;
			static uint32_t lastTick = 0;
			const ULONGLONG now = GetTickCount64();
			if (now - lastPace >= 10000)
			{
				const uint64_t frames = s.frameValue;
				if (lastPace && s.rendererState == 1)
				{
					Totals t;
					{
						std::lock_guard lock(g_timingMutex);
						std::swap(t, g_totals);
					}
					const double n = std::max(t.frames, 1u);
					log::Info("ue5 bridge: renderer {:.1f} frames/s, game {:.1f}; the game's queue waited {:.1f} ms a frame for the renderer ({:.0f}% of "
					          "frames), came to the wait {:.1f} ms after the camera, the renderer's frame done {:.1f} ms after it",
					    (frames - lastFrames) * 1000.0 / (now - lastPace), (g_tick - lastTick) * 1000.0 / (now - lastPace), t.waited / n,
					    100.0 * t.stalled / n, t.toWait / n, t.latency / n);
				}
				lastPace = now, lastFrames = frames, lastTick = g_tick;
			}
			// Console commands for the renderer: the preset's, then ModLoader/ue5console.txt (tuning while it runs),
			// again when either changes.
			if (g_tick % 60 == 0)
			{
				std::error_code ec;
				const auto file = paths::Get().root / L"ue5console.txt";
				const auto stamp = std::filesystem::last_write_time(file, ec);
				const std::string& preset = config::Get().ue5Preset;
				if ((!ec && stamp != g_consoleStamp) || preset != g_presetSent)
				{
					g_consoleStamp = ec ? std::filesystem::file_time_type{} : stamp;
					g_presetSent = preset;
					std::string text = PresetCommands(preset);
					if (!ec)
					{
						std::ifstream in(file);
						text.append((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
					}
					text.resize(std::min(text.size(), sizeof(s.console) - 1));
					std::memset(s.console, 0, sizeof(s.console));
					std::memcpy(s.console, text.data(), text.size());
					MemoryBarrier();
					++s.consoleSequence;
					log::Info("ue5 bridge: console commands sent ({} bytes)", text.size());
				}
			}
			s.enabled = g_showing ? 1 : 0;
			// Map data groups: twice a second.
			if (g_tick % 30 == 0)
			{
				uint32_t count = 0;
				for (const std::string& name : g_groups)
					if (count < GTAVBridge::kMaxGroups && natives::Invoke<int32_t>(kIsIplActive, name.c_str()))
						s.groupsOn[count++] = [&] {
							uint32_t h = 0;
							for (const char c : name)
							{
								h += static_cast<uint8_t>(std::tolower(static_cast<unsigned char>(c)));
								h += h << 10;
								h ^= h >> 6;
							}
							h += h << 3;
							h ^= h >> 11;
							return h + (h << 15);
						}();
				if (count != s.groupCount || g_tick == 0)
					log::Info("ue5 bridge: {} map data groups on", count);
				s.groupCount = count;
				++s.groupsSequence;
			}
		}

		// The camera the game renders this frame with (render thread, from Streamline): the renderer's next view.
		void OnCamera(const streamline::Camera& camera)
		{
			GTAVBridge::Shared* s = g_shared;
			if (!s || !camera.valid)
				return;
			const uint32_t sequence = s->gameSequence + 1;
			GTAVBridge::View& v = s->views[sequence % GTAVBridge::kViews];
			v.sequence = sequence;
			std::memcpy(v.position, camera.position, sizeof(v.position));
			// (the game gives Streamline the camera's back vector as cameraFwd: it looks down its -z)
			for (int i = 0; i < 3; ++i)
				v.forward[i] = -camera.forward[i];
			std::memcpy(v.up, camera.up, sizeof(v.up));
			v.fov = camera.fov * 57.29578f;
			v.aspect = camera.aspect;
			v.nearClip = camera.nearZ;
			v.farClip = camera.farZ;
			v.lights = s->lightSequence;
			g_cameraTime[sequence % GTAVBridge::kViews] = v.time = Now();
			MemoryBarrier();
			s->gameSequence = sequence;
			t_sequence = sequence;
			t_camera = camera;
			if (static bool told = false; !told)
			{
				told = true;
				const float* m = camera.viewToClip;
				log::Info("ue5 bridge: the game's projection, third and fourth rows {:.5f} {:.5f} {:.5f} {:.5f} / {:.5f} {:.5f} {:.5f} {:.5f}", m[8], m[9], m[10],
				    m[11], m[12], m[13], m[14], m[15]);
			}
		}
	}

	void Start()
	{
		PrepareGameSettings();
		if (!config::Get().ue5Renderer)
			return;
		g_key = KeyFromName(config::Get().ue5Key);
		g_showing = true;
		streamline::SetAfterDlss(&AfterDlss);
		streamline::SetOnCamera(&OnCamera);
		log::Info("ue5 bridge: on (needs the game's DLSS on; {} switches pictures)", config::Get().ue5Key);
	}

	void Tick()
	{
		// (the game has read its settings by its first DLSS frame)
		if (g_width)
			RestoreGameSettings();
		if (!config::Get().ue5Renderer || g_failed || !state::story || state::online)
			return;
		if (!g_shared)
		{
			// (the size comes from the game's first DLSS pass)
			if (!g_width || !g_height)
				return;
			if (!CreateShared() || !Launch())
			{
				g_failed = true;
				log::Error("ue5 bridge: off (see above)");
				return;
			}
		}
		if (g_process && WaitForSingleObject(g_process, 0) == WAIT_OBJECT_0)
		{
			DWORD code = 0;
			GetExitCodeProcess(g_process, &code);
			log::Error("ue5 bridge: the renderer stopped (exit code {:#x}); the game's own picture until {} starts it again", code, config::Get().ue5Key);
			CloseHandle(g_process);
			g_process = nullptr;
			g_shared->rendererState = 2;
			// (a wait the game's queue may have on a frame that never came: let it go)
			std::lock_guard lock(g_renderMutex);
			if (g_fence)
				g_fence->Signal(~0ull >> 1);
			// (a new renderer counts its textures from 1 again: these must not pass for its own)
			for (ID3D12Resource*& t : g_texture)
				Retire(t), t = nullptr;
			Retire(g_fence);
			Retire(g_consumed);
			g_fence = g_consumed = nullptr;
			g_composed.clear();
			g_openedSequence = 0;
			return;
		}
		if (!g_process)
		{
			if (!g_restart.exchange(false))
				return;
			g_shared->rendererState = 0;
			g_shared->textureSequence = 0;
			g_shared->frameValue = 0;
			if (!Launch())
				return;
		}
		Write();
		++g_tick;
	}

	void OnKeyDown(uint32_t vk)
	{
		if (!config::Get().ue5Renderer || vk != g_key)
			return;
		if (g_shared && !g_process)
		{
			g_restart = true;
			g_showing = true;
			log::Info("ue5 bridge: starting the renderer again");
			return;
		}
		g_showing = !g_showing;
		log::Info("ue5 bridge: {} picture", g_showing ? "UE5's" : "the game's");
	}

	void Stop()
	{
		if (g_process)
		{
			TerminateProcess(g_process, 0);
			CloseHandle(g_process);
			g_process = nullptr;
		}
		if (g_job)
			CloseHandle(g_job);
		g_job = nullptr;
	}

	void OnPresent(ID3D12CommandQueue* queue)
	{
		// (the oldest frame composed: its lists are all on the queue before the present)
		std::lock_guard lock(g_renderMutex);
		if (g_composed.empty() || !g_consumed)
			return;
		const uint64_t value = g_composed.front();
		g_composed.erase(g_composed.begin());
		if (value > g_consumedValue)
		{
			g_consumedValue = value;
			queue->Signal(g_consumed, value);
		}
	}

	bool Showing()
	{
		return g_showing && g_shared && g_shared->rendererState == 1;
	}

	void DumpDepth(const std::wstring& path)
	{
		std::lock_guard lock(g_dumpMutex);
		g_dumpPath = path;
	}

	void DumpColor(const std::wstring& path)
	{
		std::lock_guard lock(g_dumpMutex);
		g_colorPath = path;
	}
}
