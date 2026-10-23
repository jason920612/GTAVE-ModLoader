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
#include <vector>

#include "../config.hpp"
#include "../game/natives.hpp"
#include "../log.hpp"
#include "../paths.hpp"
#include "../state.hpp"
#include "../ui/dx12_hook.hpp"
#include "GTAVBridgeProtocol.h"
#include "streamline.hpp"

namespace loader::graphics::bridge
{
	namespace
	{
		namespace natives = game::natives;

		constexpr uint64_t kGetClockHours = 0x25223CA6B4D20B7F,
		                   kGetClockMinutes = 0x13D2B8ADD79640F2, kGetClockSeconds = 0x494E97C2EF27C470,
		                   kGetCurrWeatherState = 0xF3BBE884A14BB413, kIsIplActive = 0x88A741E44A2B3495;

		// Game thread.
		HANDLE g_mapping = nullptr;
		GTAVBridge::Shared* g_shared = nullptr;
		HANDLE g_process = nullptr, g_job = nullptr;
		bool g_failed = false;
		std::vector<std::string> g_groups; // map data group names (the pack's groups.txt)
		uint32_t g_tick = 0;
		std::filesystem::file_time_type g_consoleStamp{};
		uint32_t g_key = VK_F7;

		// Render threads: the picture shown, the size DLSS gives, the renderer's texture and fence on the game's device.
		std::atomic_bool g_showing = false;
		std::atomic_bool g_restart = false; // the hotkey while the renderer is not running
		std::atomic<uint32_t> g_width = 0, g_height = 0;
		std::mutex g_renderMutex;
		ID3D12Resource* g_texture = nullptr;
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
			if (sequence == g_openedSequence && g_texture && g_fence)
				return true;
			Retire(g_texture);
			Retire(g_fence);
			g_texture = nullptr;
			g_fence = nullptr;
			g_openedSequence = sequence;
			if (!sequence)
				return false;
			ID3D12Device1* device1 = nullptr;
			if (FAILED(device->QueryInterface(IID_PPV_ARGS(&device1))))
				return false;
			HANDLE texture = nullptr, fence = nullptr;
			bool ok = SUCCEEDED(device1->OpenSharedHandleByName(Named(GTAVBridge::kTextureName).c_str(), GENERIC_ALL, &texture)) &&
			          SUCCEEDED(device1->OpenSharedHandleByName(Named(GTAVBridge::kFenceName).c_str(), GENERIC_ALL, &fence));
			ok = ok && SUCCEEDED(device->OpenSharedHandle(texture, IID_PPV_ARGS(&g_texture))) && SUCCEEDED(device->OpenSharedHandle(fence, IID_PPV_ARGS(&g_fence)));
			if (texture)
				CloseHandle(texture);
			if (fence)
				CloseHandle(fence);
			device1->Release();
			log::Info("ue5 bridge: renderer texture {} ({}x{})", ok ? "opened" : "could not be opened", g_shared->textureWidth, g_shared->textureHeight);
			return ok;
		}

		// Right after the game's DLSS pass: the renderer's newest frame over DLSS's output, once the game's queue has
		// waited for it.
		void AfterDlss(void* commandList, void* output, uint32_t outputState)
		{
			auto* target = static_cast<ID3D12Resource*>(output);
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
			const D3D12_RESOURCE_DESC source = g_texture->GetDesc();
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
			queue->Wait(g_fence, g_shared->frameValue);
			auto* list = static_cast<ID3D12GraphicsCommandList*>(commandList);
			D3D12_RESOURCE_BARRIER barrier{};
			barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
			barrier.Transition.pResource = target;
			barrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
			barrier.Transition.StateBefore = static_cast<D3D12_RESOURCE_STATES>(outputState);
			barrier.Transition.StateAfter = D3D12_RESOURCE_STATE_COPY_DEST;
			list->ResourceBarrier(1, &barrier);
			list->CopyResource(target, g_texture);
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
			s.width = g_width, s.height = g_height;
			// Console commands for the renderer (tuning while it runs): ModLoader/ue5console.txt when it changes.
			if (g_tick % 60 == 0)
			{
				std::error_code ec;
				const auto file = paths::Get().root / L"ue5console.txt";
				const auto stamp = std::filesystem::last_write_time(file, ec);
				if (!ec && stamp != g_consoleStamp)
				{
					g_consoleStamp = stamp;
					std::ifstream in(file);
					std::string text((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
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
			++s->gameSequence;
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
}
