#include "bridge.hpp"

#include <Windows.h>
#include <d3d12.h>

#include <algorithm>
#include <atomic>
#include <chrono>
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
#include "GTAVBridgeProtocol.h"
#include "composite.hpp"
#include "streamline.hpp"

namespace loader::graphics::bridge
{
	namespace
	{
		namespace natives = game::natives;

		constexpr uint64_t kGetClockHours = 0x25223CA6B4D20B7F,
		                   kGetClockMinutes = 0x13D2B8ADD79640F2, kGetClockSeconds = 0x494E97C2EF27C470,
		                   kGetCurrWeatherState = 0xF3BBE884A14BB413, kIsIplActive = 0x88A741E44A2B3495,
		                   kGetWindSpeed = 0xA8CF1CC0AFCD3F12, kGetWindDirection = 0x1F400FEF721170DA;

		// Game thread.
		HANDLE g_mapping = nullptr;
		GTAVBridge::Shared* g_shared = nullptr;
		HANDLE g_process = nullptr, g_job = nullptr;
		bool g_failed = false;
		std::vector<std::string> g_groups; // map data group names (the pack's groups.txt)
		uint32_t g_tick = 0;
		std::filesystem::file_time_type g_consoleStamp{};
		std::string g_presetSent; // the preset the renderer has
		uint32_t g_key = VK_F7;

		// Render threads: the picture shown, the size DLSS gives, the renderer's texture and fence on the game's device.
		std::atomic_bool g_showing = false;
		std::atomic_bool g_restart = false; // the hotkey while the renderer is not running
		std::atomic<uint32_t> g_width = 0, g_height = 0;
		std::mutex g_renderMutex;
		ID3D12Resource* g_texture[2] = {}; // (the renderer's frame with fence value v is in g_texture[v % 2])
		ID3D12Fence* g_fence = nullptr;
		uint32_t g_openedSequence = 0;
		std::vector<std::pair<std::chrono::steady_clock::time_point, IUnknown*>> g_retired; // released a while later
		bool g_warnedSize = false;

		std::filesystem::path Path(const std::string& utf8)
		{
			return std::filesystem::path(reinterpret_cast<const char8_t*>(utf8.c_str()));
		}

		uint32_t KeyFromName(const std::string& name)
		{
			if (name.size() >= 2 && (name[0] == 'F' || name[0] == 'f'))
				if (const int n = std::atoi(name.c_str() + 1); n >= 1 && n <= 24)
					return VK_F1 + n - 1;
			return name.size() == 1 ? static_cast<uint32_t>(std::toupper(static_cast<unsigned char>(name[0]))) : VK_F7;
		}

		std::wstring Named(const wchar_t* prefix)
		{
			return prefix + std::to_wstring(GetCurrentProcessId());
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
			if (sequence == g_openedSequence && g_texture[0] && g_texture[1] && g_fence)
				return true;
			for (ID3D12Resource*& t : g_texture)
				Retire(t), t = nullptr;
			Retire(g_fence);
			g_fence = nullptr;
			g_openedSequence = sequence;
			if (!sequence)
				return false;
			ID3D12Device1* device1 = nullptr;
			if (FAILED(device->QueryInterface(IID_PPV_ARGS(&device1))))
				return false;
			HANDLE fence = nullptr;
			bool ok = SUCCEEDED(device1->OpenSharedHandleByName(Named(GTAVBridge::kFenceName).c_str(), GENERIC_ALL, &fence)) &&
			          SUCCEEDED(device->OpenSharedHandle(fence, IID_PPV_ARGS(&g_fence)));
			if (fence)
				CloseHandle(fence);
			for (int i = 0; i < 2 && ok; ++i)
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

		// The game's frame this render thread records (its camera's sequence number): the renderer's frame to wait for.
		thread_local uint32_t t_sequence = 0;
		uint64_t g_lastValue = 0;      // the renderer's newest frame seen, and when it last moved on
		ULONGLONG g_lastProgress = 0;

		// Right after the game's DLSS pass: the renderer's frame of this camera over DLSS's output, once the game's
		// queue has waited for it (frame for frame, so that the game's peds and vehicles sit on the renderer's
		// picture exactly; while the renderer stalls, its newest frame instead).
		void AfterDlss(const streamline::DlssFrame& frame)
		{
			DumpDepth(frame);
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
			const uint64_t value = !stalled && t_sequence > newest ? t_sequence : newest;
			queue->Wait(g_fence, value);
			ID3D12Resource* picture = g_texture[value % 2];
			// (the game's peds and vehicles kept; a plain copy when that cannot be done)
			if (composite::Record(frame, picture, std::clamp(config::Get().ue5ExposureMatch, 0.0f, 1.0f)))
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
			struct Level
			{
				const char* name;
				int resolution, scalability;
				float distance;
				int lights;
			};
			static constexpr Level kLevels[] = {
				{"low", 50, 0, 0.8f, 32}, {"medium", 67, 1, 1.0f, 64}, {"high", 80, 2, 1.25f, 128}, {"ultra", 100, 3, 1.5f, 256}, {"cinematic", 100, 4, 2.0f, 512}};
			const Level* l = &kLevels[2];
			for (const Level& k : kLevels)
				if (preset == k.name)
					l = &k;
			std::string out = std::format("r.ScreenPercentage {}\n", l->resolution);
			for (const char* group : {"ViewDistance", "Shadow", "GlobalIllumination", "Reflection", "PostProcess", "Texture", "Effects", "Foliage", "Shading"})
				out += std::format("sg.{}Quality {}\n", group, l->scalability);
			out += std::format("gtav.LodScale {}\ngtav.Lights {}\n", l->distance, l->lights);
			return out;
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
					log::Info("ue5 bridge: renderer {:.1f} frames/s, game {:.1f}", (frames - lastFrames) * 1000.0 / (now - lastPace),
					    (g_tick - lastTick) * 1000.0 / (now - lastPace));
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
			std::memcpy(s->position, camera.position, sizeof(s->position));
			// (the game gives Streamline the camera's back vector as cameraFwd: it looks down its -z)
			for (int i = 0; i < 3; ++i)
				s->forward[i] = -camera.forward[i];
			std::memcpy(s->up, camera.up, sizeof(s->up));
			s->fov = camera.fov * 57.29578f;
			s->aspect = camera.aspect;
			s->nearClip = camera.nearZ;
			s->farClip = camera.farZ;
			MemoryBarrier();
			t_sequence = ++s->gameSequence;
		}
	}

	void Start()
	{
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
			g_fence = nullptr;
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

	bool Showing()
	{
		return g_showing && g_shared && g_shared->rendererState == 1;
	}

	void DumpDepth(const std::wstring& path)
	{
		std::lock_guard lock(g_dumpMutex);
		g_dumpPath = path;
	}
}
