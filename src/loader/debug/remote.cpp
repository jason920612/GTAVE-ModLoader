#include "remote.hpp"

#include <Windows.h>
#include <MinHook.h>
#include <audiopolicy.h>
#include <mmdeviceapi.h>

#include <algorithm>
#include <atomic>
#include <cctype>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <map>
#include <mutex>
#include <set>
#include <sstream>
#include <string>
#include <thread>

#include "../config.hpp"
#include "../game/natives.hpp"
#include "../graphics/bridge.hpp"
#include "../log.hpp"
#include "../paths.hpp"
#include "../state.hpp"
#include "../ui/overlay.hpp"

namespace loader::debug::remote
{
	namespace
	{
		constexpr uint64_t kSetControlValueNextFrame = 0xE8A25867FBA3B05E;
		// A fixed camera for comparing pictures ("cam"): a scripted camera, the streaming focus there, noon, clear sky.
		constexpr uint64_t kCreateCam = 0xC3981DCE61D9E13F, kSetCamCoord = 0x4D41783FB745E42E, kSetCamRot = 0x85973643155D0B07,
		                   kSetCamFov = 0xB13C14F66A00D047, kSetCamActive = 0x026FB97D0A425F84, kRenderScriptCams = 0x07E5B515DB0636FC,
		                   kDestroyCam = 0x865908C81A2C22E9, kSetFocusPosAndVel = 0xBB7454BAFF08FE25, kClearFocus = 0x31B73D1EA9F01DA2,
		                   kSetClockTime = 0x47C3B5848C3E45D8, kPauseClock = 0x4055E40BD2DBEC1D, kSetWeatherTypeNowPersist = 0xED712CA327900C8A,
		                   kClearWeatherTypePersist = 0xCCC39339BEF76CF5;
		constexpr uint64_t kPlayerPedId = 0xD80958FC74E988A6, kSetEntityCoords = 0x06843DA7060A026B;
		struct CamRequest
		{
			bool pending = false, on = false;
			float x = 0, y = 0, z = 0, heading = 0, pitch = 0, fov = 50;
		};
		constexpr wchar_t kPipeName[] = L"\\\\.\\pipe\\gtave-modloader";

		struct Held
		{
			float value = 1.0f;
			ULONGLONG until = 0;
		};
		std::mutex g_mutex;
		std::map<int, Held> g_held; // control -> how long
		CamRequest g_cam;            // (the game thread applies it)
		bool g_teleport = false;
		float g_teleportTo[3] = {};
		int32_t g_camHandle = 0;

		// Keys for the game itself, frontend menus included (they pause scripts, so controls cannot reach them). The
		// game reads the keyboard through raw input: a WM_INPUT is posted whose handle is ours, and GetRawInputData
		// answers it with the keyboard event.
		constexpr uintptr_t kFakeInput = 0x4D4C0000; // | sequence
		struct FakeKey
		{
			uint16_t vk;
			bool up;
		};
		std::mutex g_fakeMutex;
		std::map<uintptr_t, FakeKey> g_fake;
		uint16_t g_fakeSequence = 0;
		using GetRawInputDataFn = UINT(WINAPI*)(HRAWINPUT, UINT, LPVOID, PUINT, UINT);
		GetRawInputDataFn g_getRawInputData = nullptr;
		std::atomic<HWND> g_keyboardTarget = nullptr; // where the game has the keyboard's raw input sent
		std::atomic<int> g_fakeServed = 0;
		using RegisterRawInputDevicesFn = BOOL(WINAPI*)(PCRAWINPUTDEVICE, UINT, UINT);
		RegisterRawInputDevicesFn g_registerRawInputDevices = nullptr;

		// The game only acts on keyboard input while its window is in front (GetForegroundWindow). For a game key it is
		// told so for a moment; and while it is not really in front it may not move or confine the person's cursor.
		std::atomic<ULONGLONG> g_fakeFocusUntil = 0;
		using GetForegroundWindowFn = HWND(WINAPI*)();
		GetForegroundWindowFn g_getForegroundWindow = nullptr;
		using ClipCursorFn = BOOL(WINAPI*)(const RECT*);
		ClipCursorFn g_clipCursor = nullptr;
		using SetCursorPosFn = BOOL(WINAPI*)(int, int);
		SetCursorPosFn g_setCursorPos = nullptr;
		HWND GameWindow();

		bool GameReallyInFront()
		{
			const HWND w = GameWindow();
			return w && g_getForegroundWindow() == w;
		}
		HWND WINAPI HookGetForegroundWindow()
		{
			if (GetTickCount64() < g_fakeFocusUntil)
				if (const HWND w = GameWindow())
					return w;
			return g_getForegroundWindow();
		}
		BOOL WINAPI HookClipCursor(const RECT* rect)
		{
			return rect && !GameReallyInFront() ? TRUE : g_clipCursor(rect);
		}
		BOOL WINAPI HookSetCursorPos(int x, int y)
		{
			return !GameReallyInFront() ? TRUE : g_setCursorPos(x, y);
		}

		template<class Fn>
		void HookApi(const char* name, void* detour, Fn& original)
		{
			void* target = nullptr;
			if (MH_CreateHookApiEx(L"user32", name, detour, reinterpret_cast<void**>(&original), &target) == MH_OK)
				MH_EnableHook(target);
			else
				log::Warn("remote: cannot hook {}", name);
		}

		BOOL WINAPI HookRegisterRawInputDevices(PCRAWINPUTDEVICE devices, UINT count, UINT size)
		{
			for (UINT i = 0; i < count; ++i)
			{
				log::Info("remote: raw input device page {:#x} usage {:#x} flags {:#x} target {}", devices[i].usUsagePage, devices[i].usUsage,
				    devices[i].dwFlags, static_cast<void*>(devices[i].hwndTarget));
				if (devices[i].usUsagePage == 1 && devices[i].usUsage == 6 && !(devices[i].dwFlags & RIDEV_REMOVE))
					g_keyboardTarget = devices[i].hwndTarget;
			}
			return g_registerRawInputDevices(devices, count, size);
		}

		UINT WINAPI HookGetRawInputData(HRAWINPUT input, UINT command, LPVOID data, PUINT size, UINT headerSize)
		{
			const auto id = reinterpret_cast<uintptr_t>(input);
			FakeKey key{};
			{
				std::lock_guard lock(g_fakeMutex);
				const auto it = (id & ~uintptr_t{0xFFFF}) == kFakeInput ? g_fake.find(id) : g_fake.end();
				if (it == g_fake.end())
					return g_getRawInputData(input, command, data, size, headerSize);
				key = it->second;
			}
			if (g_fakeServed++ < 8)
				log::Info("remote: game key {:#x} {} read (command {:#x}, buffer {})", key.vk, key.up ? "up" : "down", command, data != nullptr);
			const UINT need = command == RID_HEADER ? sizeof(RAWINPUTHEADER) : sizeof(RAWINPUT);
			if (!size)
				return static_cast<UINT>(-1);
			if (!data)
			{
				*size = need;
				return 0;
			}
			if (*size < need)
			{
				SetLastError(ERROR_INSUFFICIENT_BUFFER);
				return static_cast<UINT>(-1);
			}
			RAWINPUT r{};
			r.header.dwType = RIM_TYPEKEYBOARD;
			r.header.dwSize = sizeof(RAWINPUT);
			r.header.wParam = RIM_INPUT;
			r.data.keyboard.MakeCode = static_cast<USHORT>(MapVirtualKeyW(key.vk, MAPVK_VK_TO_VSC));
			const bool extended = key.vk == VK_UP || key.vk == VK_DOWN || key.vk == VK_LEFT || key.vk == VK_RIGHT;
			r.data.keyboard.Flags = static_cast<USHORT>((key.up ? RI_KEY_BREAK : RI_KEY_MAKE) | (extended ? RI_KEY_E0 : 0));
			r.data.keyboard.VKey = key.vk;
			r.data.keyboard.Message = key.up ? WM_KEYUP : WM_KEYDOWN;
			std::memcpy(data, &r, need);
			return need;
		}

		bool PostGameKey(HWND window, uint32_t vk, bool up)
		{
			if (!g_getRawInputData)
			{
				void* target = nullptr;
				if (MH_CreateHookApiEx(L"user32", "GetRawInputData", reinterpret_cast<void*>(&HookGetRawInputData),
				        reinterpret_cast<void**>(&g_getRawInputData), &target) != MH_OK ||
				    MH_EnableHook(target) != MH_OK)
					return false;
			}
			uintptr_t id = 0;
			{
				std::lock_guard lock(g_fakeMutex);
				id = kFakeInput | ++g_fakeSequence;
				g_fake[id] = {static_cast<uint16_t>(vk), up};
				while (g_fake.size() > 64)
					g_fake.erase(g_fake.begin());
			}
			const HWND target = g_keyboardTarget ? g_keyboardTarget.load() : window;
			return PostMessageW(target, WM_INPUT, RIM_INPUT, static_cast<LPARAM>(id)) != 0;
		}

		RECT g_restore{};
		std::atomic_bool g_offscreen = false;
		std::atomic_bool g_keepShown = false; // the tester asked for the window back: leave it

		uint32_t ParseKey(std::string name)
		{
			std::transform(name.begin(), name.end(), name.begin(), [](unsigned char c) { return static_cast<char>(std::toupper(c)); });
			if (name.size() == 1 && std::isalnum(static_cast<unsigned char>(name[0])))
				return static_cast<uint32_t>(name[0]);
			if (name.size() >= 2 && name[0] == 'F' && std::all_of(name.begin() + 1, name.end(), ::isdigit))
			{
				const int n = std::stoi(name.substr(1));
				return n >= 1 && n <= 24 ? static_cast<uint32_t>(VK_F1 + n - 1) : 0;
			}
			static const std::map<std::string, uint32_t> named = {{"ENTER", VK_RETURN}, {"ESC", VK_ESCAPE}, {"BACK", VK_BACK}, {"SPACE", VK_SPACE},
			    {"UP", VK_UP}, {"DOWN", VK_DOWN}, {"LEFT", VK_LEFT}, {"RIGHT", VK_RIGHT}, {"TAB", VK_TAB}};
			if (const auto it = named.find(name); it != named.end())
				return it->second;
			try
			{
				return static_cast<uint32_t>(std::stoul(name, nullptr, 0));
			}
			catch (...)
			{
				return 0;
			}
		}

		// Gives the keyboard back to the window the person was using (the game was put in front when it started).
		void HandFocusBack(HWND game)
		{
			if (GetForegroundWindow() != game)
				return;
			for (HWND w = GetWindow(game, GW_HWNDFIRST); w; w = GetWindow(w, GW_HWNDNEXT))
			{
				if (w == game || !IsWindowVisible(w) || IsIconic(w) || GetWindow(w, GW_OWNER) || GetWindowTextLengthW(w) == 0 ||
				    (GetWindowLongW(w, GWL_EXSTYLE) & WS_EX_TOOLWINDOW))
					continue;
				SetForegroundWindow(w); // allowed: the game is the foreground process
				return;
			}
		}

		// The game's window: the overlay's, or before it attached, the process's visible top-level window.
		HWND GameWindow()
		{
			if (const HWND w = ui::GameWindow())
				return w;
			struct Find
			{
				HWND best = nullptr;
				LONG area = 0;
			} find;
			EnumWindows(
			    [](HWND w, LPARAM lp) -> BOOL {
				    DWORD pid = 0;
				    GetWindowThreadProcessId(w, &pid);
				    RECT r{};
				    if (pid == GetCurrentProcessId() && IsWindowVisible(w) && !GetWindow(w, GW_OWNER) && GetWindowRect(w, &r))
				    {
					    auto* f = reinterpret_cast<Find*>(lp);
					    if (const LONG a = (r.right - r.left) * (r.bottom - r.top); a > f->area)
						    f->best = w, f->area = a;
				    }
				    return TRUE;
			    },
			    reinterpret_cast<LPARAM>(&find));
			return find.best;
		}

		std::string Window(const std::string& how)
		{
			const HWND w = GameWindow();
			if (!w)
				return "err no game window yet";
			if (how == "offscreen")
			{
				if (!g_offscreen)
					GetWindowRect(w, &g_restore);
				g_offscreen = true;
				g_keepShown = false;
				// Right of every monitor, not minimised (a minimised window stops presenting), never activated.
				const int x = GetSystemMetrics(SM_XVIRTUALSCREEN) + GetSystemMetrics(SM_CXVIRTUALSCREEN) + 200;
				SetWindowPos(w, HWND_BOTTOM, x, 0, 0, 0, SWP_NOSIZE | SWP_NOACTIVATE);
				HandFocusBack(w);
				return "ok";
			}
			if (how == "restore")
			{
				if (g_offscreen)
					SetWindowPos(w, nullptr, g_restore.left, g_restore.top, 0, 0, SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE);
				g_offscreen = false;
				g_keepShown = true;
				return "ok";
			}
			return "err window offscreen|restore";
		}

		// Windows keeps an app's mute in the volume mixer for its next starts too, so the output devices a test muted
		// are noted (ModLoader\remote_muted, device ids) until it is undone - when the game next starts without test
		// control if the test never got to it (the game killed or crashed).
		std::filesystem::path MutedFile()
		{
			return paths::Get().root / L"remote_muted";
		}
		std::set<std::wstring> ReadMuted()
		{
			std::set<std::wstring> ids;
			std::ifstream in(MutedFile());
			for (std::string line; std::getline(in, line);)
				if (!line.empty())
				{
					std::wstring id;
					for (const char c : line)
						id += static_cast<wchar_t>(static_cast<unsigned char>(c));
					ids.insert(id);
				}
			return ids;
		}
		void WriteMuted(const std::set<std::wstring>& ids)
		{
			std::error_code ec;
			if (ids.empty())
			{
				std::filesystem::remove(MutedFile(), ec);
				return;
			}
			std::ofstream out(MutedFile(), std::ios::trunc);
			for (const auto& id : ids)
			{
				for (const wchar_t c : id) // device ids are ASCII
					out << static_cast<char>(c);
				out << '\n';
			}
		}

		// The game's own audio session (the volume mixer's entry) on every output device, so a test running beside the
		// person is silent. Off undoes only what a test did: the person may have muted the game on a device themselves.
		std::string Mute(bool on)
		{
			const HRESULT co = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
			std::set<std::wstring> muted = ReadMuted();
			bool any = false;
			IMMDeviceEnumerator* devices = nullptr;
			IMMDeviceCollection* all = nullptr;
			UINT count = 0;
			if (SUCCEEDED(CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL, IID_PPV_ARGS(&devices))) &&
			    SUCCEEDED(devices->EnumAudioEndpoints(eRender, DEVICE_STATE_ACTIVE, &all)))
				all->GetCount(&count);
			for (UINT i = 0; i < count; ++i)
			{
				IMMDevice* device = nullptr;
				LPWSTR id = nullptr;
				IAudioSessionManager* sessions = nullptr;
				ISimpleAudioVolume* volume = nullptr;
				BOOL was = FALSE;
				if (SUCCEEDED(all->Item(i, &device)) && SUCCEEDED(device->GetId(&id)) &&
				    SUCCEEDED(device->Activate(__uuidof(IAudioSessionManager), CLSCTX_ALL, nullptr, reinterpret_cast<void**>(&sessions))) &&
				    SUCCEEDED(sessions->GetSimpleAudioVolume(nullptr, FALSE, &volume)) && SUCCEEDED(volume->GetMute(&was)))
				{
					any = true;
					if (on && !was && SUCCEEDED(volume->SetMute(TRUE, nullptr)))
						muted.insert(id);
					else if (!on && muted.count(id) && SUCCEEDED(volume->SetMute(FALSE, nullptr)))
						muted.erase(id);
				}
				if (volume)
					volume->Release();
				if (sessions)
					sessions->Release();
				CoTaskMemFree(id);
				if (device)
					device->Release();
			}
			WriteMuted(muted);
			if (all)
				all->Release();
			if (devices)
				devices->Release();
			if (SUCCEEDED(co))
				CoUninitialize();
			return any ? "ok" : "err no audio session";
		}

		std::string Run(const std::string& line)
		{
			std::istringstream in(line);
			std::string cmd;
			in >> cmd;
			if (cmd == "ping")
				return std::string("ok ") + (state::online ? "online" : state::story ? "story" : state::landing ? "landing" : "loading");
			if (cmd == "story")
			{
				// The home screen's "continue story" (the click the UI would get).
				if (!state::landing || !state::canContinueStory)
					return "err not on the home screen";
				state::storyLoading = true;
				state::storyRequested = true;
				return "ok";
			}
			if (cmd == "key")
			{
				std::string name;
				in >> name;
				const uint32_t vk = ParseKey(name);
				const HWND w = GameWindow();
				if (!vk || !w)
					return vk ? "err no game window yet" : "err unknown key";
				PostMessageW(w, kKeyMessage, vk, 0);
				return "ok";
			}
			if (cmd == "gamekey")
			{
				std::string name;
				int ms = 100;
				in >> name >> ms;
				const uint32_t vk = ParseKey(name);
				const HWND w = GameWindow();
				if (!vk || !w)
					return vk ? "err no game window yet" : "err unknown key";
				g_fakeFocusUntil = GetTickCount64() + static_cast<ULONGLONG>(std::clamp(ms, 1, 5000)) + 300;
				// Frontend menus (pause, save) and replay recording also want the game to think it is the active app.
				// It is told so and left so: telling it "inactive" afterwards stops a replay recording and the injected
				// controls. Raw input from the person's real keyboard and mouse still goes only to the real foreground
				// window, and the cursor hooks above keep the cursor free while the game is not really in front.
				if (!GameReallyInFront())
				{
					PostMessageW(w, WM_ACTIVATEAPP, TRUE, 0);
					PostMessageW(w, WM_ACTIVATE, WA_ACTIVE, 0);
					Sleep(50);
				}
				if (!PostGameKey(w, vk, false))
					return "err raw input hook";
				Sleep(static_cast<DWORD>(std::clamp(ms, 1, 5000)));
				PostGameKey(w, vk, true);
				return "ok";
			}
			if (cmd == "control")
			{
				int id = -1, ms = 120;
				float value = 1.0f;
				in >> id >> ms >> value;
				if (id < 0 || id > 400)
					return "err control id";
				std::lock_guard lock(g_mutex);
				g_held[id] = {value, GetTickCount64() + static_cast<ULONGLONG>(std::max(ms, 1))};
				return "ok";
			}
			if (cmd == "fakefocus")
			{
				// GetForegroundWindow reports the game for this long.
				int ms = 0;
				in >> ms;
				g_fakeFocusUntil = GetTickCount64() + static_cast<ULONGLONG>(std::clamp(ms, 0, 120000));
				return "ok";
			}
			if (cmd == "release")
			{
				std::lock_guard lock(g_mutex);
				g_held.clear();
				return "ok";
			}
			if (cmd == "shot")
			{
				std::string path;
				std::getline(in >> std::ws, path);
				if (path.empty())
					return "err shot <path>";
				const int wide = MultiByteToWideChar(CP_UTF8, 0, path.c_str(), -1, nullptr, 0);
				std::wstring wpath(static_cast<size_t>(wide), L'\0');
				MultiByteToWideChar(CP_UTF8, 0, path.c_str(), -1, wpath.data(), wide);
				wpath.resize(wcslen(wpath.c_str()));
				std::string error;
				return ui::Capture(wpath, error) ? "ok" : "err " + error;
			}
			if (cmd == "depthdump")
			{
				std::string path;
				std::getline(in >> std::ws, path);
				const int wide = MultiByteToWideChar(CP_UTF8, 0, path.c_str(), -1, nullptr, 0);
				std::wstring wpath(static_cast<size_t>(wide), L'\0');
				MultiByteToWideChar(CP_UTF8, 0, path.c_str(), -1, wpath.data(), wide);
				wpath.resize(wcslen(wpath.c_str()));
				graphics::bridge::DumpDepth(wpath);
				return "ok";
			}
			if (cmd == "tp")
			{
				// tp x y z: the player there
				std::lock_guard lock(g_mutex);
				if (!(in >> g_teleportTo[0] >> g_teleportTo[1] >> g_teleportTo[2]))
					return "err tp x y z";
				g_teleport = true;
				return "ok";
			}
			if (cmd == "cam")
			{
				// cam x y z heading pitch [fov]  |  cam off
				CamRequest r;
				std::string first;
				in >> first;
				r.pending = true;
				r.on = first != "off";
				if (r.on)
				{
					r.x = std::stof(first);
					in >> r.y >> r.z >> r.heading >> r.pitch;
					if (!(in >> r.fov))
						r.fov = 50;
				}
				std::lock_guard lock(g_mutex);
				g_cam = r;
				return "ok";
			}
			if (cmd == "mute")
			{
				std::string how;
				in >> how;
				return Mute(how != "off");
			}
			if (cmd == "window")
			{
				std::string how;
				in >> how;
				return Window(how);
			}
			return "err unknown command";
		}

		void Client(HANDLE pipe)
		{
			std::string buffer;
			char chunk[1024];
			DWORD got = 0;
			while (ReadFile(pipe, chunk, sizeof chunk, &got, nullptr) && got)
			{
				buffer.append(chunk, got);
				for (size_t nl; (nl = buffer.find('\n')) != std::string::npos;)
				{
					std::string line = buffer.substr(0, nl);
					buffer.erase(0, nl + 1);
					if (!line.empty() && line.back() == '\r')
						line.pop_back();
					if (line.empty())
						continue;
					const std::string reply = Run(line) + "\n";
					DWORD put = 0;
					WriteFile(pipe, reply.data(), static_cast<DWORD>(reply.size()), &put, nullptr);
				}
			}
			DisconnectNamedPipe(pipe);
			CloseHandle(pipe);
		}

		// Any number of testers at once (a script and the test player), each on its own thread.
		void Serve()
		{
			for (;;)
			{
				const HANDLE pipe = CreateNamedPipeW(kPipeName, PIPE_ACCESS_DUPLEX, PIPE_TYPE_BYTE | PIPE_READMODE_BYTE | PIPE_WAIT | PIPE_REJECT_REMOTE_CLIENTS,
				    PIPE_UNLIMITED_INSTANCES, 4096, 4096, 0, nullptr);
				if (pipe == INVALID_HANDLE_VALUE)
				{
					log::Error("remote: pipe unavailable ({})", GetLastError());
					return;
				}
				if (ConnectNamedPipe(pipe, nullptr) || GetLastError() == ERROR_PIPE_CONNECTED)
					std::thread(Client, pipe).detach();
				else
					CloseHandle(pipe);
			}
		}
	}

	void Start()
	{
		if (!config::Get().debugRemote)
		{
			// A test's mute left behind: the sound back on.
			if (!ReadMuted().empty())
				std::thread([] {
					while (!GameWindow())
						Sleep(250);
					if (Mute(false) == "ok")
						log::Info("remote: the game's sound is back on (a test had muted it)");
				}).detach();
			return;
		}
		std::thread(Serve).detach();
		// Where the game's keyboard input goes, and its view of being in front (for gamekey); the person's cursor.
		HookApi("RegisterRawInputDevices", reinterpret_cast<void*>(&HookRegisterRawInputDevices), g_registerRawInputDevices);
		HookApi("GetForegroundWindow", reinterpret_cast<void*>(&HookGetForegroundWindow), g_getForegroundWindow);
		HookApi("ClipCursor", reinterpret_cast<void*>(&HookClipCursor), g_clipCursor);
		HookApi("SetCursorPos", reinterpret_cast<void*>(&HookSetCursorPos), g_setCursorPos);
		// Out of view and silent from the moment the window shows, and kept out of view (the game moves its window
		// back when it applies its display settings).
		std::thread([] {
			bool muted = false;
			for (;;)
			{
				Sleep(250);
				const HWND w = GameWindow();
				if (!w)
					continue;
				if (!muted)
					muted = Mute(true) == "ok";
				RECT r{};
				GetWindowRect(w, &r);
				const int right = GetSystemMetrics(SM_XVIRTUALSCREEN) + GetSystemMetrics(SM_CXVIRTUALSCREEN);
				if (!g_keepShown && r.left < right)
				{
					Window("offscreen");
					log::Info("remote: game window moved out of view");
				}
			}
		}).detach();
		log::Warn("remote: test control is on (\\\\.\\pipe\\gtave-modloader)");
	}

	void Tick()
	{
		const ULONGLONG now = GetTickCount64();
		std::lock_guard lock(g_mutex);
		for (auto it = g_held.begin(); it != g_held.end();)
		{
			if (now >= it->second.until)
			{
				it = g_held.erase(it);
				continue;
			}
			game::natives::Invoke<int>(kSetControlValueNextFrame, 0, it->first, it->second.value);
			++it;
		}
		if (g_teleport)
		{
			g_teleport = false;
			game::natives::Invoke<void>(kSetEntityCoords, game::natives::Invoke<int32_t>(kPlayerPedId), g_teleportTo[0], g_teleportTo[1], g_teleportTo[2], 0, 0, 0, 1);
			log::Info("remote: player to {:.1f} {:.1f} {:.1f}", g_teleportTo[0], g_teleportTo[1], g_teleportTo[2]);
		}
		if (g_cam.pending)
		{
			namespace natives = game::natives;
			g_cam.pending = false;
			if (g_cam.on)
			{
				if (!g_camHandle)
					g_camHandle = natives::Invoke<int32_t>(kCreateCam, "DEFAULT_SCRIPTED_CAMERA", 0);
				natives::Invoke<void>(kSetCamCoord, g_camHandle, g_cam.x, g_cam.y, g_cam.z);
				natives::Invoke<void>(kSetCamRot, g_camHandle, g_cam.pitch, 0.0f, g_cam.heading, 2);
				natives::Invoke<void>(kSetCamFov, g_camHandle, g_cam.fov);
				natives::Invoke<void>(kSetCamActive, g_camHandle, 1);
				natives::Invoke<void>(kRenderScriptCams, 1, 0, 0, 1, 0, 0);
				natives::Invoke<void>(kSetFocusPosAndVel, g_cam.x, g_cam.y, g_cam.z, 0.0f, 0.0f, 0.0f);
				natives::Invoke<void>(kSetClockTime, 12, 0, 0);
				natives::Invoke<void>(kPauseClock, 1);
				natives::Invoke<void>(kSetWeatherTypeNowPersist, "EXTRASUNNY");
				log::Info("remote: camera at {:.1f} {:.1f} {:.1f} heading {:.1f} pitch {:.1f} fov {:.1f}", g_cam.x, g_cam.y, g_cam.z, g_cam.heading, g_cam.pitch,
				    g_cam.fov);
			}
			else if (g_camHandle)
			{
				natives::Invoke<void>(kRenderScriptCams, 0, 0, 0, 1, 0, 0);
				natives::Invoke<void>(kDestroyCam, g_camHandle, 0);
				natives::Invoke<void>(kClearFocus);
				natives::Invoke<void>(kPauseClock, 0);
				natives::Invoke<void>(kClearWeatherTypePersist);
				g_camHandle = 0;
			}
		}
	}
}
