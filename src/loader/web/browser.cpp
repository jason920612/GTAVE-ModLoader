#include "browser.hpp"

#include <d3d12.h>

#include <atomic>
#include <cstring>
#include <deque>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <thread>
#include <vector>

#include <imgui.h>
#include <nlohmann/json.hpp>

#include "../../browser/mlbrowser.h"
#include "textures.hpp"
#include "../config.hpp"
#include "../game/natives.hpp"
#include "../game/script.hpp"
#include "../game/scripts.hpp"
#include "../log.hpp"
#include "../mods.hpp"
#include "../paths.hpp"
#include "../state.hpp"

namespace loader::web
{
	namespace
	{
		constexpr char kHome[] = "https://www.eyefind.info/";
		constexpr uint32_t kBrowserOpenGlobal = 77414; // appinternet runs while this is set

		// ---- mlbrowser.dll -----------------------------------------------------------------

		struct Dll
		{
			MLB_InitFn init = nullptr;
			MLB_OpenFn open = nullptr;
			MLB_HideFn hide = nullptr;
			MLB_FrameFn frame = nullptr;
			MLB_MouseMoveFn mouseMove = nullptr;
			MLB_MouseButtonFn mouseButton = nullptr;
			MLB_WheelFn wheel = nullptr;
			MLB_KeyFn key = nullptr;
			MLB_BackFn back = nullptr;
			MLB_ForwardFn forward = nullptr;
			MLB_UrlFn url = nullptr;
			MLB_RespondFn respond = nullptr;
			MLB_EmitFn emit = nullptr;
			MLB_TextureReadyFn textureReady = nullptr;
		} g_dll;

		enum class Status
		{
			NotLoaded,
			Starting,
			Ready,
			Failed
		};
		std::atomic<Status> g_status = Status::NotLoaded;
		std::atomic_bool g_open = false;
		std::mutex g_startMutex;
		std::string g_pendingUrl; // opened once CEF is up (start mutex)

		// View size (page area) in pixels, as the overlay last laid it out.
		std::atomic<int> g_viewWidth = 1280, g_viewHeight = 720, g_toolbar = 56;
		std::atomic<int> g_mouseX = 0, g_mouseY = 0;
		std::atomic_bool g_editingUrl = false;

		// Page calls waiting for the game thread.
		struct Query
		{
			int64_t id;
			std::string request;
		};
		std::mutex g_queryMutex;
		std::deque<Query> g_queries;

		void OnQuery(int64_t id, const char* request)
		{
			std::lock_guard lock(g_queryMutex);
			g_queries.push_back({id, request ? request : ""});
		}
		void OnTexture(int64_t id, const char* dictionary, const char* texture)
		{
			textures::Request(dictionary ? dictionary : "", texture ? texture : "", [id](const std::wstring& file) {
				std::string path(file.size() * 3, '\0');
				path.resize(WideCharToMultiByte(CP_UTF8, 0, file.c_str(), static_cast<int>(file.size()), path.data(), static_cast<int>(path.size()), nullptr, nullptr));
				g_dll.textureReady(id, path.c_str());
			});
		}
		void OnLog(int level, const char* text)
		{
			switch (level)
			{
			case 0: log::Debug("{}", text); break;
			case 1: log::Info("{}", text); break;
			case 2: log::Warn("{}", text); break;
			default: log::Error("{}", text); break;
			}
		}

		template<class T>
		bool Resolve(HMODULE module, const char* name, T& out)
		{
			out = reinterpret_cast<T>(GetProcAddress(module, name));
			return out != nullptr;
		}

		// Loads mlbrowser.dll and starts CEF. Background thread.
		void Start()
		{
			const auto dir = paths::Get().root / L"browser";
			HMODULE module = LoadLibraryExW((dir / L"mlbrowser.dll").c_str(), nullptr, LOAD_WITH_ALTERED_SEARCH_PATH);
			if (!module)
			{
				log::Error("browser: cannot load {} (error {}); is ModLoader\\browser installed?", (dir / L"mlbrowser.dll").string(), GetLastError());
				g_status = Status::Failed;
				return;
			}
			bool ok = Resolve(module, "MLB_Init", g_dll.init) && Resolve(module, "MLB_Open", g_dll.open) && Resolve(module, "MLB_Hide", g_dll.hide) &&
			          Resolve(module, "MLB_Frame", g_dll.frame) && Resolve(module, "MLB_MouseMove", g_dll.mouseMove) &&
			          Resolve(module, "MLB_MouseButton", g_dll.mouseButton) && Resolve(module, "MLB_Wheel", g_dll.wheel) &&
			          Resolve(module, "MLB_Key", g_dll.key) && Resolve(module, "MLB_Back", g_dll.back) && Resolve(module, "MLB_Forward", g_dll.forward) &&
			          Resolve(module, "MLB_Url", g_dll.url) && Resolve(module, "MLB_Respond", g_dll.respond) && Resolve(module, "MLB_Emit", g_dll.emit) &&
			          Resolve(module, "MLB_TextureReady", g_dll.textureReady);
			if (!ok)
			{
				log::Error("browser: mlbrowser.dll does not match this loader");
				g_status = Status::Failed;
				return;
			}

			// Page roots: the loader's own sites, then each mod's web folder.
			std::vector<std::wstring> roots{(paths::Get().root / L"web").wstring()};
			std::error_code ec;
			for (const auto& entry : std::filesystem::directory_iterator(paths::Get().mods, ec))
				if (entry.is_directory(ec) && std::filesystem::is_directory(entry.path() / L"web", ec))
					roots.push_back((entry.path() / L"web").wstring());
			std::vector<const wchar_t*> rootPtrs;
			for (const auto& r : roots)
				rootPtrs.push_back(r.c_str());
			const std::wstring browserDir = dir.wstring(), cacheDir = (dir / L"cache").wstring();

			MLBConfig cfg{};
			cfg.browserDir = browserDir.c_str();
			cfg.cacheDir = cacheDir.c_str();
			cfg.webRoots = rootPtrs.data();
			cfg.webRootCount = static_cast<int>(rootPtrs.size());
			cfg.locale = "zh-TW";
			cfg.callbacks = {&OnQuery, &OnLog, &OnTexture};
			if (!g_dll.init(&cfg))
			{
				g_status = Status::Failed;
				return;
			}
			log::Info("browser: ready ({} page folder(s))", roots.size());
			std::lock_guard lock(g_startMutex);
			g_status = Status::Ready;
			if (g_open)
				g_dll.open(g_pendingUrl.c_str(), g_viewWidth, g_viewHeight);
		}

		// ---- page frame on the GPU (render thread) --------------------------------------------

		struct Gpu
		{
			ID3D12Resource* texture = nullptr;
			std::vector<ID3D12Resource*> uploads; // one per back buffer
			std::vector<void*> mapped;
			UINT pitch = 0;
			int width = 0, height = 0;
			D3D12_GPU_DESCRIPTOR_HANDLE srv{};
			uint32_t serial = 0;
			bool hasFrame = false;
			std::vector<std::pair<ID3D12Resource*, unsigned>> retired; // released after a few frames
		} g_gpu;

		void Retire(ID3D12Resource* r)
		{
			if (r)
				g_gpu.retired.push_back({r, 8});
		}

		bool CreateTexture(ID3D12Device* device, int width, int height, unsigned frameCount, DescriptorAlloc alloc)
		{
			Retire(g_gpu.texture);
			for (auto* u : g_gpu.uploads)
				Retire(u);
			g_gpu = Gpu{.retired = std::move(g_gpu.retired)};

			D3D12_HEAP_PROPERTIES heap{D3D12_HEAP_TYPE_DEFAULT};
			D3D12_RESOURCE_DESC desc{};
			desc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
			desc.Width = width;
			desc.Height = height;
			desc.DepthOrArraySize = 1;
			desc.MipLevels = 1;
			desc.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
			desc.SampleDesc.Count = 1;
			if (FAILED(device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &desc, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE, nullptr,
			        IID_PPV_ARGS(&g_gpu.texture))))
				return false;

			g_gpu.pitch = (width * 4 + D3D12_TEXTURE_DATA_PITCH_ALIGNMENT - 1) & ~(D3D12_TEXTURE_DATA_PITCH_ALIGNMENT - 1);
			D3D12_HEAP_PROPERTIES upload{D3D12_HEAP_TYPE_UPLOAD};
			D3D12_RESOURCE_DESC buffer{};
			buffer.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
			buffer.Width = UINT64(g_gpu.pitch) * height;
			buffer.Height = 1;
			buffer.DepthOrArraySize = 1;
			buffer.MipLevels = 1;
			buffer.SampleDesc.Count = 1;
			buffer.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
			for (unsigned i = 0; i < frameCount; ++i)
			{
				ID3D12Resource* r = nullptr;
				void* p = nullptr;
				if (FAILED(device->CreateCommittedResource(&upload, D3D12_HEAP_FLAG_NONE, &buffer, D3D12_RESOURCE_STATE_GENERIC_READ, nullptr, IID_PPV_ARGS(&r))) ||
				    FAILED(r->Map(0, nullptr, &p)))
					return false;
				g_gpu.uploads.push_back(r);
				g_gpu.mapped.push_back(p);
			}

			D3D12_CPU_DESCRIPTOR_HANDLE cpu{};
			if (!alloc(&cpu, &g_gpu.srv))
				return false;
			D3D12_SHADER_RESOURCE_VIEW_DESC view{};
			view.Format = desc.Format;
			view.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
			view.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
			view.Texture2D.MipLevels = 1;
			device->CreateShaderResourceView(g_gpu.texture, &view, cpu);
			g_gpu.width = width;
			g_gpu.height = height;
			return true;
		}

		std::string CurrentUrl()
		{
			if (g_status != Status::Ready)
				return {};
			char buf[1024];
			g_dll.url(buf, sizeof(buf));
			return buf;
		}
	}

	void Open(const std::string& url)
	{
		std::lock_guard lock(g_startMutex);
		g_open = true;
		switch (g_status)
		{
		case Status::NotLoaded:
			g_status = Status::Starting;
			g_pendingUrl = url;
			std::thread(Start).detach();
			break;
		case Status::Starting:
			g_pendingUrl = url;
			break;
		case Status::Ready:
			g_dll.open(url.c_str(), g_viewWidth, g_viewHeight);
			break;
		case Status::Failed:
			g_open = false;
			break;
		}
	}

	void Close()
	{
		g_open = false;
		if (g_status == Status::Ready)
			g_dll.hide();
	}

	bool IsOpen()
	{
		return g_open;
	}

	void Respond(int64_t id, bool ok, const char* result)
	{
		if (g_status == Status::Ready)
			g_dll.respond(id, ok ? 1 : 0, result);
	}

	void Emit(const char* event, const char* json)
	{
		if (g_status == Status::Ready)
			g_dll.emit(event, json);
	}

	void Tick()
	{
		// The game's browser (phone and computers) is replaced by this one. It is told to close (Global 77414, its
		// "open" flag) so it ends through its own clean-up and the phone takes it as closed; ours covers it meanwhile.
		static int32_t replaced = 0;
		if (config::Get().replaceBrowser)
		{
			if (auto* thread = game::script::FindThread(game::script::Joaat("appinternet")))
			{
				const int32_t id = *reinterpret_cast<int32_t*>(reinterpret_cast<uint8_t*>(thread) + 0x08);
				if (int64_t* open = game::scripts::Global(kBrowserOpenGlobal))
					*open = 0;
				if (id != replaced)
				{
					replaced = id;
					log::Info("browser: the game's browser was opened (thread {}); showing ours", id);
					Open(kHome);
				}
			}
		}

		textures::Tick();

		// Development aid: ModLoaderrowser_open.txt holds a URL to open.
		static uint64_t nextCheck = 0;
		if (GetTickCount64() >= nextCheck)
		{
			nextCheck = GetTickCount64() + 1000;
			const auto trigger = paths::Get().root / L"browser_open.txt";
			std::error_code ec;
			if (std::filesystem::exists(trigger, ec))
			{
				std::string url;
				std::getline(std::ifstream(trigger), url);
				std::filesystem::remove(trigger, ec);
				if (!url.empty())
					Open(url);
			}
		}

		std::deque<Query> queries;
		{
			std::lock_guard lock(g_queryMutex);
			queries.swap(g_queries);
		}
		for (auto& q : queries)
		{
			const auto request = nlohmann::json::parse(q.request, nullptr, false);
			if (!request.is_object() || !request.contains("name") || !request["name"].is_string())
			{
				Respond(q.id, false, "bad request");
				continue;
			}
			const std::string name = request["name"];
			const std::string args = request.contains("args") ? request["args"].dump() : "[]";
			if (name == "browser.close")
			{
				Close();
				Respond(q.id, true, nullptr);
			}
			else if (!mods::CallWebFunction(name, args, q.id))
				Respond(q.id, false, ("unknown function: " + name).c_str());
		}
	}

	void Upload(ID3D12Device* device, ID3D12GraphicsCommandList* list, unsigned frame, unsigned frameCount, DescriptorAlloc alloc)
	{
		for (auto it = g_gpu.retired.begin(); it != g_gpu.retired.end();)
			if (--it->second == 0)
			{
				it->first->Release();
				it = g_gpu.retired.erase(it);
			}
			else
				++it;
		if (!g_open || g_status != Status::Ready)
			return;
		const int width = g_viewWidth, height = g_viewHeight;
		if (width <= 0 || height <= 0)
			return;
		if ((g_gpu.width != width || g_gpu.height != height || g_gpu.uploads.size() != frameCount) &&
		    !CreateTexture(device, width, height, frameCount, alloc))
		{
			log::Error("browser: could not create the page texture");
			Close();
			return;
		}
		if (!g_dll.frame(&g_gpu.serial, g_gpu.mapped[frame], static_cast<int>(g_gpu.pitch), width, height))
			return;
		D3D12_RESOURCE_BARRIER barrier{};
		barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
		barrier.Transition.pResource = g_gpu.texture;
		barrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
		barrier.Transition.StateBefore = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
		barrier.Transition.StateAfter = D3D12_RESOURCE_STATE_COPY_DEST;
		list->ResourceBarrier(1, &barrier);
		D3D12_TEXTURE_COPY_LOCATION dst{g_gpu.texture, D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX};
		dst.SubresourceIndex = 0;
		D3D12_TEXTURE_COPY_LOCATION src{g_gpu.uploads[frame], D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT};
		src.PlacedFootprint.Footprint = {DXGI_FORMAT_B8G8R8A8_UNORM, static_cast<UINT>(width), static_cast<UINT>(height), 1, g_gpu.pitch};
		list->CopyTextureRegion(&dst, 0, 0, 0, &src, nullptr);
		std::swap(barrier.Transition.StateBefore, barrier.Transition.StateAfter);
		list->ResourceBarrier(1, &barrier);
		g_gpu.hasFrame = true;
	}

	void Draw()
	{
		if (!g_open)
		{
			g_editingUrl = false;
			return;
		}
		const ImGuiIO& io = ImGui::GetIO();
		const float scale = io.DisplaySize.y / 1080.0f;
		const int toolbar = static_cast<int>(56 * scale);
		g_toolbar = toolbar;
		g_viewWidth = static_cast<int>(io.DisplaySize.x);
		g_viewHeight = static_cast<int>(io.DisplaySize.y) - toolbar;

		ImGui::SetNextWindowPos({0, 0});
		ImGui::SetNextWindowSize(io.DisplaySize);
		ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, {0, 0});
		ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 0);
		ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 0);
		ImGui::PushStyleColor(ImGuiCol_WindowBg, IM_COL32(40, 40, 40, 255));
		ImGui::Begin("##browser", nullptr,
		    ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoBringToFrontOnFocus |
		        ImGuiWindowFlags_NoScrollWithMouse);

		// Toolbar: back, forward, home, address, close.
		const float pad = 10 * scale, button = toolbar - 2 * pad;
		ImGui::SetCursorPos({pad, pad});
		const bool ready = g_status == Status::Ready;
		if (ImGui::Button("<", {button, button}) && ready)
			g_dll.back();
		ImGui::SameLine();
		if (ImGui::Button(">", {button, button}) && ready)
			g_dll.forward();
		ImGui::SameLine();
		if (ImGui::Button("首頁##home", {button * 1.6f, button}) && ready)
			g_dll.open(kHome, g_viewWidth, g_viewHeight);
		ImGui::SameLine();
		static char address[512];
		static bool editing = false;
		if (!editing)
		{
			std::string url = CurrentUrl();
			for (const char* prefix : {"https://", "http://"})
				if (url.starts_with(prefix))
					url.erase(0, std::strlen(prefix));
			if (url.ends_with("/"))
				url.pop_back();
			strncpy_s(address, url.c_str(), _TRUNCATE);
		}
		ImGui::SetNextItemWidth(io.DisplaySize.x - ImGui::GetCursorPosX() - button - 3 * pad);
		ImGui::SetCursorPosY(pad + (button - ImGui::GetFrameHeight()) / 2);
		if (ImGui::InputText("##address", address, sizeof(address), ImGuiInputTextFlags_EnterReturnsTrue) && ready)
		{
			std::string target = address;
			if (target.find("://") == std::string::npos)
				target = "https://" + target;
			g_dll.open(target.c_str(), g_viewWidth, g_viewHeight);
		}
		editing = ImGui::IsItemActive();
		g_editingUrl = editing;
		ImGui::SameLine();
		ImGui::SetCursorPos({io.DisplaySize.x - button - pad, pad});
		if (ImGui::Button("X", {button, button}))
			Close();

		// The page.
		ImGui::SetCursorPos({0, static_cast<float>(toolbar)});
		if (g_gpu.hasFrame && g_gpu.texture)
			ImGui::Image(ImTextureRef(static_cast<ImTextureID>(g_gpu.srv.ptr)), {static_cast<float>(g_gpu.width), static_cast<float>(g_gpu.height)});
		else
			ImGui::TextDisabled("...");
		ImGui::End();
		ImGui::PopStyleColor();
		ImGui::PopStyleVar(3);
	}

	bool OnMessage(HWND, UINT msg, WPARAM wp, LPARAM lp)
	{
		if (!g_open)
			return false;
		const bool ready = g_status == Status::Ready;
		const int toolbar = g_toolbar;
		switch (msg)
		{
		case WM_MOUSEMOVE:
		case WM_LBUTTONDOWN:
		case WM_LBUTTONUP:
		case WM_RBUTTONDOWN:
		case WM_RBUTTONUP:
		case WM_MBUTTONDOWN:
		case WM_MBUTTONUP:
		{
			const int x = static_cast<short>(LOWORD(lp)), y = static_cast<short>(HIWORD(lp)) - toolbar;
			g_mouseX = x;
			g_mouseY = y;
			if (!ready || (y < 0 && msg != WM_LBUTTONUP && msg != WM_RBUTTONUP && msg != WM_MBUTTONUP))
				return true;
			if (msg == WM_MOUSEMOVE)
				g_dll.mouseMove(x, y);
			else
			{
				const int button = (msg == WM_LBUTTONDOWN || msg == WM_LBUTTONUP) ? 0 : (msg == WM_MBUTTONDOWN || msg == WM_MBUTTONUP) ? 1 : 2;
				const bool down = msg == WM_LBUTTONDOWN || msg == WM_RBUTTONDOWN || msg == WM_MBUTTONDOWN;
				g_dll.mouseButton(x, y, button, down);
			}
			return true;
		}
		case WM_MOUSEWHEEL:
			if (ready && g_mouseY >= 0)
				g_dll.wheel(g_mouseX, g_mouseY, 0, GET_WHEEL_DELTA_WPARAM(wp));
			return true;
		case WM_KEYDOWN:
		case WM_SYSKEYDOWN:
			// Esc: back, or close on the first page (like the game's browser).
			if (wp == VK_ESCAPE && !g_editingUrl)
			{
				if (!(lp & (1 << 30)) && (!ready || !g_dll.back()))
					Close();
				return true;
			}
			[[fallthrough]];
		case WM_KEYUP:
		case WM_SYSKEYUP:
		case WM_CHAR:
		case WM_SYSCHAR:
			if (ready && !g_editingUrl && !(wp == VK_ESCAPE))
				g_dll.key(msg, wp, lp);
			return true;
		default:
			return (msg >= WM_KEYFIRST && msg <= WM_KEYLAST) || (msg >= WM_MOUSEFIRST && msg <= WM_MOUSELAST) || msg == WM_INPUT;
		}
	}
}
