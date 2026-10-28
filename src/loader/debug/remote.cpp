#include "remote.hpp"

#include <Windows.h>
#include <MinHook.h>
#include <audiopolicy.h>
#include <mmdeviceapi.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <cctype>
#include <cmath>
#include <condition_variable>
#include <cstring>
#include <deque>
#include <filesystem>
#include <functional>
#include <format>
#include <fstream>
#include <map>
#include <memory>
#include <mutex>
#include <set>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

#include "../config.hpp"
#include "../convert/resource.hpp"
#include "../game/content.hpp"
#include "../game/dlcpacks.hpp"
#include "../game/landfiles.hpp"
#include "../game/seamless.hpp"
#include "../game/natives.hpp"
#include "../game/sectors.hpp"
#include "../graphics/bridge.hpp"
#include "../graphics/particles.hpp"
#include "../log.hpp"
#include "../mods.hpp"
#include "../paths.hpp"
#include "../state.hpp"
#include "../ui/overlay.hpp"
#include "../ui/recorder.hpp"
#include "profile.hpp"
#include "watchdog.hpp"

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
		                   kClearWeatherTypePersist = 0xCCC39339BEF76CF5, kSetClockDate = 0xB096419DF0D06CE7, kSetRain = 0x643E26EA6E024D92;
		constexpr uint64_t kPlayerPedId = 0xD80958FC74E988A6, kSetEntityCoords = 0x06843DA7060A026B;
		constexpr uint64_t kGetGameplayCamRelativeHeading = 0x743607648ADD4587, kSetGameplayCamRelativeHeading = 0xB4EC2312F4E5B1F1;
		struct CamRequest
		{
			bool pending = false, on = false, keep = false; // (keep: the clock and weather as they are)
			float x = 0, y = 0, z = 0, heading = 0, pitch = 0, fov = 50;
		};
		constexpr wchar_t kPipeName[] = L"\\\\.\\pipe\\gtave-modloader";

		struct Held
		{
			float value = 1.0f;
			ULONGLONG until = 0;
		};
		std::mutex g_mutex;

		// Research: each data file a change set enables (0xC39440: its entry - +0 the data file, +8 its kind, +0xC enable)
		// timed while `g_timeDataFiles` (the data file's name at +0x10 as the game keeps it - logged as it is)
		using EnableDataFileFn = uint64_t (*)(uint8_t* entry);
		EnableDataFileFn g_enableDataFile = nullptr;
		bool g_timeDataFiles = false;
		std::vector<std::string> g_dataFileTimes;

		uint64_t EnableDataFileHook(uint8_t* entry)
		{
			if (!g_timeDataFiles)
				return g_enableDataFile(entry);
			const auto t = std::chrono::steady_clock::now();
			const uint64_t r = g_enableDataFile(entry);
			const double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t).count();
			const auto* file = *reinterpret_cast<const uint8_t* const*>(entry);
			char name[96] = "?";
			if (file)
				strncpy_s(name, reinterpret_cast<const char*>(file), _TRUNCATE);
			g_dataFileTimes.push_back(std::format("{} kind {} {:.1f} ms", name, *reinterpret_cast<const uint32_t*>(entry + 8), ms));
			return r;
		}

		std::map<int, Held> g_held; // control -> how long
		// "call": a mod's web function called as a page would (game.call), its answer back to the tester
		struct ModCall
		{
			std::string name, args;
			int64_t id = 0;
			bool queued = false, done = false, ok = false;
			std::string result;
		};
		std::deque<std::shared_ptr<ModCall>> g_modCalls; // waiting for the game thread
		std::map<int64_t, std::shared_ptr<ModCall>> g_modCallsOut; // queued on a mod, waiting for its answer
		std::condition_variable g_modCallDone;
		int64_t g_nextModCall = -1; // (negative: the browser's ids are positive)
		CamRequest g_cam;            // (the game thread applies it)
		bool g_teleport = false;
		float g_teleportTo[3] = {};
		int g_hour = -1, g_minute = 0; // "time": the clock set and stopped there (or left running: the game moves its sun
		bool g_clockRuns = false;      // only while the clock runs)
		int g_date[3] = {};            // "date": day, month (1-12), year
		std::string g_weather;         // "weather": that type, kept
		float g_spinRate = 0;          // "spin": the gameplay camera turned this fast (degrees a second) until then
		ULONGLONG g_spinUntil = 0, g_spinLast = 0;

		// "native": a call the game thread makes for the tester (arguments: integers, floats with a '.', s:text).
		struct NativeCall
		{
			uint64_t hash = 0;
			std::vector<uint64_t> args;
			std::vector<std::string> strings; // (what string arguments point into)
			uint64_t result[3]{};
			uint64_t out[8][3]{}; // ("out" arguments: what the native wrote there)
			int outs = 0;
			bool done = false, ok = false;
		};
		std::deque<std::shared_ptr<NativeCall>> g_calls;
		// "dlc": work the game thread does for the tester, its answer back
		struct Job
		{
			std::function<std::string()> run;
			std::string result;
			bool done = false;
		};
		std::deque<std::shared_ptr<Job>> g_jobs;
		std::vector<std::pair<std::shared_ptr<NativeCall>, ULONGLONG>> g_repeats; // ("every": until then, each frame)
		std::condition_variable g_callDone;
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
			if (cmd == "record")
			{
				// record <seconds> <fps> <scale> <path> | record status
				std::string first;
				in >> first;
				if (first == "status")
					return ui::recorder::Busy() ? "ok recording" : "ok done";
				double seconds = 0, fps = 30;
				int scale = 2;
				std::string path;
				try
				{
					seconds = std::stod(first);
				}
				catch (...)
				{
					return "err record <seconds> <fps> <scale> <path>";
				}
				in >> fps >> scale;
				std::getline(in >> std::ws, path);
				if (path.empty())
					return "err record <seconds> <fps> <scale> <path>";
				const int wide = MultiByteToWideChar(CP_UTF8, 0, path.c_str(), -1, nullptr, 0);
				std::wstring wpath(static_cast<size_t>(wide), L'\0');
				MultiByteToWideChar(CP_UTF8, 0, path.c_str(), -1, wpath.data(), wide);
				wpath.resize(wcslen(wpath.c_str()));
				std::string error;
				return ui::recorder::Start(wpath, seconds, fps, scale, error) ? "ok" : "err " + error;
			}
			if (cmd == "native" || cmd == "every")
			{
				// native <hash> [args]: the result as an integer, as a float, and as a vector (x y z)
				// every <seconds> <hash> [args] | every off: called each frame that long (the DRAW_* natives draw one frame)
				auto call = std::make_shared<NativeCall>();
				std::string word;
				double seconds = 0;
				if (cmd == "every")
				{
					if (!(in >> word))
						return "err every <seconds> <hash> [args] | every off";
					if (word == "off")
					{
						std::lock_guard lock(g_mutex);
						g_repeats.clear();
						return "ok";
					}
					seconds = std::clamp(std::stod(word), 0.0, 600.0);
				}
				if (!(in >> word))
					return "err native <hash> [args]";
				try
				{
					call->hash = std::stoull(word, nullptr, 0);
				}
				catch (...)
				{
					return "err hash";
				}
				std::vector<std::string> words;
				while (in >> word)
					words.push_back(word);
				call->strings.reserve(words.size());
				for (const std::string& w : words)
				{
					uint64_t slot = 0;
					if (w.rfind("s:", 0) == 0)
					{
						call->strings.push_back(w.substr(2));
						const char* text = call->strings.back().c_str();
						std::memcpy(&slot, &text, sizeof(text));
					}
					else if ((w == "out" || w.rfind("out:", 0) == 0) && call->outs < 8)
					{
						uint64_t* at = call->out[call->outs++];
						if (w.size() > 4) // out:<n>: the native reads n there first (DELETE_VEHICLE(&vehicle))
							try
							{
								at[0] = static_cast<uint64_t>(std::stoll(w.substr(4), nullptr, 0));
							}
							catch (...)
							{
								return "err argument " + w;
							}
						std::memcpy(&slot, &at, sizeof(at));
					}
					else
						try
						{
							if (w.find('.') != std::string::npos)
							{
								const float f = std::stof(w);
								std::memcpy(&slot, &f, sizeof(f));
							}
							else
							{
								const int64_t i = std::stoll(w, nullptr, 0);
								std::memcpy(&slot, &i, sizeof(i));
							}
						}
						catch (...)
						{
							return "err argument " + w;
						}
					call->args.push_back(slot);
				}
				std::unique_lock lock(g_mutex);
				if (cmd == "every")
				{
					g_repeats.emplace_back(call, GetTickCount64() + static_cast<ULONGLONG>(seconds * 1000.0));
					return "ok";
				}
				g_calls.push_back(call);
				if (!g_callDone.wait_for(lock, std::chrono::seconds(5), [&] { return call->done; }))
					return "err the game thread did not run it";
				if (!call->ok)
					return "err the call failed";
				float f = 0, v[3];
				std::memcpy(&f, &call->result[0], 4);
				for (int k = 0; k < 3; ++k)
					std::memcpy(&v[k], &call->result[k], 4);
				std::string reply = std::format("ok {} {} {} {} {}", static_cast<int64_t>(call->result[0]), f, v[0], v[1], v[2]);
				for (int k = 0; k < call->outs; ++k)
				{
					float of = 0, ov[3];
					std::memcpy(&of, &call->out[k][0], 4);
					for (int c = 0; c < 3; ++c)
						std::memcpy(&ov[c], &call->out[k][c], 4);
					reply += std::format(" | out {} {} {} {} {}", static_cast<int64_t>(call->out[k][0]), of, ov[0], ov[1], ov[2]);
				}
				return reply;
			}
			if (cmd == "handler")
			{
				// handler <hash>: where a native's code is (research: what it reads and writes)
				std::string word;
				in >> word;
				uint64_t hash = 0;
				try
				{
					hash = std::stoull(word, nullptr, 0);
				}
				catch (...)
				{
					return "err handler <hash>";
				}
				const auto handler = reinterpret_cast<uintptr_t>(game::natives::FindHandler(hash));
				const auto base = reinterpret_cast<uintptr_t>(GetModuleHandleW(nullptr));
				return handler ? std::format("ok {:#x} rva {:#x}", handler, handler - base) : "err unknown native";
			}
			if (cmd == "spin")
			{
				// spin <degrees a second> <seconds>: the gameplay camera turned round the player
				float rate = 0, seconds = 0;
				if (!(in >> rate >> seconds))
					return "err spin <degrees a second> <seconds>";
				std::lock_guard lock(g_mutex);
				g_spinRate = rate;
				g_spinUntil = GetTickCount64() + static_cast<ULONGLONG>(std::clamp(seconds, 0.0f, 600.0f) * 1000.0f);
				g_spinLast = 0;
				return "ok";
			}
			if (cmd == "decals" || cmd == "particles")
			{
				// decals 0|1, particles 0|1: the game's decals (particles) over the renderer's picture or not
				int on = 1;
				in >> on;
				(cmd == "decals" ? graphics::particles::SetDecals : graphics::particles::SetParticles)(on != 0);
				return "ok";
			}
			if (cmd == "ptfxtrace")
			{
				std::string path;
				std::getline(in >> std::ws, path);
				graphics::particles::Trace(std::filesystem::path(std::u8string(path.begin(), path.end())).wstring());
				return "ok";
			}
			if (cmd == "depthdump" || cmd == "colordump")
			{
				std::string path;
				std::getline(in >> std::ws, path);
				const int wide = MultiByteToWideChar(CP_UTF8, 0, path.c_str(), -1, nullptr, 0);
				std::wstring wpath(static_cast<size_t>(wide), L'\0');
				MultiByteToWideChar(CP_UTF8, 0, path.c_str(), -1, wpath.data(), wide);
				wpath.resize(wcslen(wpath.c_str()));
				(cmd == "depthdump" ? graphics::bridge::DumpDepth : graphics::bridge::DumpColor)(wpath);
				return "ok";
			}
			if (cmd == "call")
			{
				// call <name> [json args]: a mod's web function (RegisterWebFunction), run on its mod's thread as a page's
				// game.call would be; the reply is "ok <its result>"
				auto call = std::make_shared<ModCall>();
				in >> call->name;
				std::getline(in, call->args);
				call->args.erase(0, call->args.find_first_not_of(' '));
				if (call->args.empty())
					call->args = "[]";
				std::unique_lock lock(g_mutex);
				call->id = g_nextModCall--;
				g_modCalls.push_back(call);
				if (!g_modCallDone.wait_for(lock, std::chrono::seconds(30), [&] { return call->done; }))
				{
					g_modCallsOut.erase(call->id);
					return "err no answer in 30 s";
				}
				return call->ok ? "ok " + call->result : "err " + call->result;
			}
			if (cmd == "peek")
			{
				// peek <address> [bytes]: memory as hex (up to 512 bytes; an address with an r in front is the game's module's
				// offset, a * in front reads the pointer there first); for looking at the game's own structures from tests
				std::string at;
				size_t count = 64;
				in >> at >> count;
				count = std::min<size_t>(count, 512);
				uintptr_t address = 0;
				bool deref = false;
				if (!at.empty() && at[0] == '*')
					deref = true, at.erase(0, 1);
				if (!at.empty() && at[0] == 'r')
					address = reinterpret_cast<uintptr_t>(GetModuleHandleW(nullptr)) + std::stoull(at.substr(1), nullptr, 16);
				else
					address = std::stoull(at, nullptr, 16);
				std::vector<uint8_t> bytes(count);
				SIZE_T got = 0;
				if (deref)
				{
					uintptr_t to = 0;
					if (!ReadProcessMemory(GetCurrentProcess(), reinterpret_cast<const void*>(address), &to, sizeof(to), &got) || got != sizeof(to))
						return "err cannot read the pointer";
					address = to;
				}
				if (!ReadProcessMemory(GetCurrentProcess(), reinterpret_cast<const void*>(address), bytes.data(), count, &got) || got != count)
					return "err cannot read there";
				std::string out = std::format("ok {:x}", address);
				for (const uint8_t b : bytes)
					out += std::format(" {:02x}", b);
				return out;
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
			if (cmd == "time")
			{
				// time h [m] [run]: the clock there, stopped (or running on)
				int h = -1, m = 0;
				if (!(in >> h) || h < 0 || h > 23)
					return "err time h [m] [run]";
				in >> m;
				std::string run;
				in.clear();
				in >> run;
				std::lock_guard lock(g_mutex);
				g_hour = h, g_minute = std::clamp(m, 0, 59);
				g_clockRuns = run == "run";
				return "ok";
			}
			if (cmd == "date")
			{
				// date d m y: the calendar there (month 1-12)
				int d = 0, m = 0, y = 0;
				if (!(in >> d >> m >> y) || d < 1 || d > 31 || m < 1 || m > 12 || y < 1)
					return "err date d m y";
				std::lock_guard lock(g_mutex);
				g_date[0] = d, g_date[1] = m, g_date[2] = y;
				return "ok";
			}
			if (cmd == "weather")
			{
				// weather EXTRASUNNY | CLEAR | CLOUDS | RAIN | THUNDER | FOGGY | ...
				std::string type;
				if (!(in >> type))
					return "err weather <type>";
				std::lock_guard lock(g_mutex);
				g_weather = type;
				return "ok";
			}
			if (cmd == "cam")
			{
				// cam x y z heading pitch [fov [keep]]  |  cam off
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
					std::string keep;
					in.clear();
					in >> keep;
					r.keep = keep == "keep";
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
			if (cmd == "shift")
			{
				// shift dx dy dz: the player (or their vehicle) moved by that much in one frame, the gameplay camera's heading
				// and pitch relative to them kept (research: a land crossed without a cut)
				float d[3]{};
				in >> d[0] >> d[1] >> d[2];
				auto job = std::make_shared<Job>();
				job->run = [d] {
					using game::natives::Invoke;
					const int32_t player = Invoke<int32_t>(0xD80958FC74E988A6ull);
					const int32_t vehicle = Invoke<int32_t>(0x997ABD671D25CA0Bull, player, 0) ? Invoke<int32_t>(0x9A9112A0FE9A4713ull, player, 0) : 0;
					const int32_t e = vehicle ? vehicle : player;
					const float heading = Invoke<float>(0x743607648ADD4587ull), pitch = Invoke<float>(0x3A6867B4845BEDA2ull);
					const float speed = Invoke<float>(0xD5037BA82E12416Full, e);
					const auto p = Invoke<game::natives::ScrVector>(0x3FEF770D40960D5Aull, e, 1);
					Invoke<void>(0x239A3351AC1DA385ull, e, p.x + d[0], p.y + d[1], p.z + d[2], 0, 0, 0);
					Invoke<void>(0xB4EC2312F4E5B1F1ull, heading);
					Invoke<void>(0x6D0858B8EDFD2B7Dull, pitch, 1.0f);
					return std::format("{:.1f} {:.1f} {:.1f} -> +({:.0f} {:.0f} {:.0f}), speed {:.1f}, camera {:.1f} {:.1f}", p.x, p.y, p.z, d[0], d[1], d[2], speed,
					    heading, pitch);
				};
				std::unique_lock lock(g_mutex);
				g_jobs.push_back(job);
				if (!g_callDone.wait_for(lock, std::chrono::seconds(30), [&] { return job->done; }))
					return "err the game thread did not run it";
				return "ok " + job->result;
			}
			if (cmd == "seamless" || cmd == "frames")
			{
				// seamless begin <land> | seamless status | frames start|stop: research (game/seamless.hpp) - a land's maps
				// registered over frames, the frames' times
				std::string what, land, there;
				float band[4]{};
				in >> what >> land >> there >> band[0] >> band[1] >> band[2] >> band[3];
				auto job = std::make_shared<Job>();
				job->run = [cmd, what, land, there, band] {
					if (cmd == "frames")
						return game::seamless::Frames(what == "start");
					if (what == "begin")
					{
						const std::string error = game::seamless::Begin(land, there == "-" ? std::string() : there, band);
						return error.empty() ? std::string("begun") : error;
					}
					return game::seamless::Status();
				};
				std::unique_lock lock(g_mutex);
				g_jobs.push_back(job);
				if (!g_callDone.wait_for(lock, std::chrono::seconds(30), [&] { return job->done; }))
					return "err the game thread did not run it";
				return "ok " + job->result;
			}
			if (cmd == "ipls")
			{
				// ipls remove|request <map> [<map> ...]: maps switched off or on by name in one go (REMOVE_IPL / REQUEST_IPL;
				// research: a land's band)
				std::string what, name;
				in >> what;
				std::vector<std::string> names;
				while (in >> name)
					names.push_back(name);
				auto job = std::make_shared<Job>();
				job->run = [what, names] {
					const auto t = std::chrono::steady_clock::now();
					int active = 0;
					for (const std::string& n : names)
					{
						game::natives::Invoke<void>(what == "remove" ? 0xEE6C5AD3ECE0A82Dull : 0x41B4893843BBDB74ull, n.c_str());
						active += game::natives::Invoke<int32_t>(0x88A741E44A2B3495ull, n.c_str()) != 0;
					}
					return std::format("{} map(s) {}d in {:.1f} ms, {} active now", names.size(), what,
					    std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t).count(), active);
				};
				std::unique_lock lock(g_mutex);
				g_jobs.push_back(job);
				if (!g_callDone.wait_for(lock, std::chrono::seconds(30), [&] { return job->done; }))
					return "err the game thread did not run it";
				return "ok " + job->result;
			}
			if (cmd == "mapsoft")
			{
				// mapsoft <hex mask>: research - calls of the content manager's map path (0xC27740) made to do nothing, a bit
				// each (0 puts them all back): which of them clear the world, which cost the time. A call rel32 (its target
				// checked) or a call through a vtable (its bytes checked).
				struct Site
				{
					uintptr_t at;
					uint8_t size;
					uintptr_t target;   // (rel32 calls)
					uint8_t bytes[3];   // (vtable calls)
				};
				static constexpr Site kSites[] = {{0xC27B8F, 5, 0x3A6370}, {0xC27B94, 5, 0x127A3A0}, {0xC27BA0, 5, 0xCEECD0}, {0xC27BB4, 5, 0x926EE0},
				    {0xC27BBB, 5, 0x112D1F0}, {0xC27BC5, 5, 0x1D94BB0}, {0xC27C45, 5, 0xC527D0}, {0xC27C63, 5, 0x926EE0}, {0xC27C73, 5, 0x935490},
				    {0xC27C7B, 5, 0x4B9EE0}, {0xC27B81, 5, 0xC2DB90}, {0xC27C6B, 5, 0xC2E0B0}, {0xC27E44, 5, 0xCF03F0},
				    {0xC27DAD, 3, 0, {0xFF, 0x50, 0x30}}, {0xC27DBE, 3, 0, {0xFF, 0x50, 0x10}},
				    // (bit 15: a data file list's "load everything asked for now" - 0xC39440's kind 4, a jump to 0x4D5110 - made a return)
				    {0xC395F6, 5, 0x4D5110}};
				static std::map<uintptr_t, std::array<uint8_t, 5>> saved;
				std::string word;
				in >> word;
				const uint32_t mask = static_cast<uint32_t>(std::stoul(word.empty() ? "0" : word, nullptr, 16));
				const auto base = reinterpret_cast<uintptr_t>(GetModuleHandleW(nullptr));
				std::string out = "ok";
				for (size_t i = 0; i < std::size(kSites); ++i)
				{
					const Site& site = kSites[i];
					auto* at = reinterpret_cast<uint8_t*>(base + site.at);
					DWORD old = 0;
					if (mask & (1u << i))
					{
						if (saved.contains(site.at))
							continue;
						bool same = false;
						if (site.size == 5)
						{
							int32_t rel = 0;
							std::memcpy(&rel, at + 1, 4);
							same = (at[0] == 0xE8 || at[0] == 0xE9) && site.at + 5 + rel == site.target;
						}
						else
							same = std::memcmp(at, site.bytes, 3) == 0;
						if (!same)
						{
							out += std::format(" (site {:#x} differs)", site.at);
							continue;
						}
						std::array<uint8_t, 5> was{};
						std::memcpy(was.data(), at, site.size);
						saved[site.at] = was;
						static constexpr uint8_t kNop5[] = {0x0F, 0x1F, 0x44, 0x00, 0x00}, kNop3[] = {0x0F, 0x1F, 0x00}, kRet5[] = {0xC3, 0x90, 0x90, 0x90, 0x90};
						VirtualProtect(at, site.size, PAGE_EXECUTE_READWRITE, &old);
						std::memcpy(at, site.size == 3 ? kNop3 : at[0] == 0xE9 ? kRet5 : kNop5, site.size);
						VirtualProtect(at, site.size, old, &old);
						out += std::format(" off {:#x}", site.at);
					}
					else if (const auto it = saved.find(site.at); it != saved.end())
					{
						VirtualProtect(at, site.size, PAGE_EXECUTE_READWRITE, &old);
						std::memcpy(at, it->second.data(), site.size);
						VirtualProtect(at, site.size, old, &old);
						saved.erase(it);
						out += std::format(" on {:#x}", site.at);
					}
					FlushInstructionCache(GetCurrentProcess(), at, site.size);
				}
				return out;
			}
			if (cmd == "execas" || cmd == "revertas")
			{
				// execas|revertas <pack folder> <group hex> <as hex>: research - a pack's change sets run through another
				// group's path (game/content.hpp); the pack mounted first if it waits
				std::string pack, group, as, profile;
				in >> pack >> group >> as >> profile;
				auto job = std::make_shared<Job>();
				job->run = [cmd, pack, group, as, profile] {
					const std::string mounted = cmd == "execas" ? game::dlcpacks::MountLater(pack) : std::string();
					const auto t = std::chrono::steady_clock::now();
					const uint32_t g = static_cast<uint32_t>(std::stoul(group, nullptr, 16)), a = static_cast<uint32_t>(std::stoul(as, nullptr, 16));
					int n = 0;
					const auto run = [&] {
						n = cmd == "execas" ? game::content::ExecuteAs(convert::Joaat(pack), g, a) : game::content::RevertAs(convert::Joaat(pack), g, a);
					};
					// (profile: sampled, by the calls of the change set's own execution or revert, 0xC27740 / 0xC27E80; files: each
					// data file enabled timed)
					std::string samples;
					if (profile == "profile")
						samples = "; " + (cmd == "execas" ? profile::Profiled(0xC27740, 0xC27E80, run) : profile::Profiled(0xC27E80, 0xC28800, run));
					else if (profile == "files")
					{
						if (!g_enableDataFile)
						{
							auto* at = reinterpret_cast<void*>(reinterpret_cast<uintptr_t>(GetModuleHandleW(nullptr)) + 0xC39440);
							if (MH_CreateHook(at, reinterpret_cast<void*>(&EnableDataFileHook), reinterpret_cast<void**>(&g_enableDataFile)) == MH_OK)
								MH_EnableHook(at);
						}
						g_dataFileTimes.clear();
						g_timeDataFiles = true;
						run();
						g_timeDataFiles = false;
						for (const std::string& d : g_dataFileTimes)
							samples += "; " + d;
					}
					else
						run();
					const double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t).count();
					return std::format("{} change set(s) in {:.1f} ms{}{}", n, ms, mounted.empty() ? "" : " (mount: " + mounted + ")", samples);
				};
				std::unique_lock lock(g_mutex);
				g_jobs.push_back(job);
				if (!g_callDone.wait_for(lock, std::chrono::seconds(60), [&] { return job->done; }))
					return "err the game thread did not run it";
				return "ok " + job->result;
			}
			if (cmd == "landboxes")
			{
				// landboxes <land>: its collision and map files' boxes in their stores (game/landfiles.hpp)
				std::string land;
				in >> land;
				auto job = std::make_shared<Job>();
				job->run = [land] { return game::landfiles::Boxes(land, paths::Get().mods / land / L"dlc.rpf"); };
				std::unique_lock lock(g_mutex);
				g_jobs.push_back(job);
				if (!g_callDone.wait_for(lock, std::chrono::seconds(30), [&] { return job->done; }))
					return "err the game thread did not run it";
				return "ok " + job->result;
			}
			if (cmd == "crash")
			{
				// crash [status] | dump [full] | resume | skip | test: the watchdog's held fault (debug/watchdog.hpp) - answered
				// here, on the pipe's thread, while the game's waits
				std::string rest;
				std::getline(in, rest);
				if (rest.find("test") != std::string::npos)
				{
					auto job = std::make_shared<Job>();
					job->run = [] { return watchdog::Test(); };
					std::lock_guard lock(g_mutex);
					g_jobs.push_back(job);
					return "ok the game thread runs into an int3 of the game's code next frame";
				}
				return watchdog::Command(rest);
			}
			if (cmd == "sector")
			{
				// sector: the world builder's lands and which are there; sector <name> [x y]: that land there instead of the
				// one in its place, the player then at x y (game/sectors.hpp; the switch runs on over the next frames, after
				// this answer)
				std::string to;
				float at[2]{};
				in >> to;
				const bool place = static_cast<bool>(in >> at[0] >> at[1]);
				if (to.empty())
				{
					std::string all;
					for (const std::string& l : game::sectors::Lands())
						all += (all.empty() ? "" : " ") + l;
					return "ok lands " + all + " | there " + game::sectors::Active();
				}
				auto job = std::make_shared<Job>();
				job->run = [to, at, place] {
					const std::string error = game::sectors::Switch(to, place ? at : nullptr);
					return error.empty() ? std::string("switching") : error;
				};
				std::unique_lock lock(g_mutex);
				g_jobs.push_back(job);
				if (!g_callDone.wait_for(lock, std::chrono::seconds(30), [&] { return job->done; }))
					return "err the game thread did not run it";
				return "ok " + job->result;
			}
			if (cmd == "dlc")
			{
				// dlc packs | dlc execute|revert <pack> <group> [execute|revert <pack> <group> ...]: the DLC content
				// manager's packs (their change set groups, what of them is executed; one line, packs split by ';'), and
				// packs' groups of change sets executed or reverted on the game thread, all in one go (names or 0x hashes;
				// research/phase0.md §62)
				std::string what;
				in >> what;
				const auto hashOf = [](const std::string& s) {
					return s.starts_with("0x") ? static_cast<uint32_t>(std::stoul(s, nullptr, 16)) : convert::Joaat(s);
				};
				auto job = std::make_shared<Job>();
				if (what == "packs")
					job->run = [] {
						std::string d = game::content::Describe();
						std::replace(d.begin(), d.end(), '\n', ';');
						return d;
					};
				else
				{
					struct Step
					{
						bool execute;
						uint32_t pack, group;
					};
					std::vector<Step> steps;
					for (std::string op = what, pack, group; (op == "execute" || op == "revert") && in >> pack >> group; op.clear(), in >> op)
						steps.push_back({op == "execute", hashOf(pack), hashOf(group)});
					if (steps.empty())
						return "err dlc packs | dlc execute|revert <pack> <group> [...]";
					job->run = [steps] {
						std::string out = "done";
						for (const Step& s : steps)
							if (!(s.execute ? game::content::Execute(s.pack, s.group) : game::content::Revert(s.pack, s.group)))
								out += std::format(" (no pack {:08x} group {:08x})", s.pack, s.group);
						return out;
					};
				}
				std::unique_lock lock(g_mutex);
				g_jobs.push_back(job);
				if (!g_callDone.wait_for(lock, std::chrono::seconds(30), [&] { return job->done; }))
					return "err the game thread did not run it";
				return "ok " + job->result;
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
					// (a malformed number anywhere is the tester's mistake, not the game's end)
					std::string reply;
					try
					{
						reply = Run(line) + "\n";
					}
					catch (const std::exception& e)
					{
						reply = std::string("err ") + e.what() + "\n";
					}
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

	void ModCallAnswered(int64_t id, bool ok, const char* result)
	{
		std::lock_guard lock(g_mutex);
		const auto it = g_modCallsOut.find(id);
		if (it == g_modCallsOut.end())
			return;
		it->second->ok = ok;
		it->second->result = result ? result : "null";
		it->second->done = true;
		g_modCallsOut.erase(it);
		g_modCallDone.notify_all();
	}

	void Tick()
	{
		const ULONGLONG now = GetTickCount64();
		std::lock_guard lock(g_mutex);
		while (!g_modCalls.empty())
		{
			const auto call = g_modCalls.front();
			g_modCalls.pop_front();
			g_modCallsOut[call->id] = call;
			if (!mods::CallWebFunction(call->name, call->args, call->id))
			{
				g_modCallsOut.erase(call->id);
				call->result = "no mod registered " + call->name;
				call->done = true;
				g_modCallDone.notify_all();
			}
		}
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
		while (!g_jobs.empty())
		{
			const std::shared_ptr<Job> job = g_jobs.front();
			g_jobs.pop_front();
			job->result = job->run();
			job->done = true;
			g_callDone.notify_all();
		}
		while (!g_calls.empty())
		{
			const std::shared_ptr<NativeCall> call = g_calls.front();
			g_calls.pop_front();
			game::natives::Invocation inv;
			inv.Begin(call->hash);
			for (const uint64_t a : call->args)
				inv.Push(a);
			call->ok = game::natives::Call(inv) == game::natives::CallStatus::Ok;
			// (a vector result is three padded floats)
			call->result[0] = inv.result[0], call->result[1] = inv.result[1], call->result[2] = inv.result[2];
			call->done = true;
			g_callDone.notify_all();
		}
		std::erase_if(g_repeats, [&](const auto& r) { return now >= r.second; });
		for (const auto& [call, until] : g_repeats)
		{
			game::natives::Invocation inv;
			inv.Begin(call->hash);
			for (const uint64_t a : call->args)
				inv.Push(a);
			game::natives::Call(inv);
		}
		if (now < g_spinUntil)
		{
			const float dt = g_spinLast ? static_cast<float>(now - g_spinLast) / 1000.0f : 0.0f;
			float heading = game::natives::Invoke<float>(kGetGameplayCamRelativeHeading) + g_spinRate * dt;
			heading = std::remainder(heading, 360.0f);
			game::natives::Invoke<void>(kSetGameplayCamRelativeHeading, heading);
			g_spinLast = now;
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
				if (!g_cam.keep)
				{
					natives::Invoke<void>(kSetClockTime, 12, 0, 0);
					natives::Invoke<void>(kPauseClock, 1);
					natives::Invoke<void>(kSetWeatherTypeNowPersist, "EXTRASUNNY");
					natives::Invoke<void>(kSetRain, 0.0f); // (no rain's puddles: a wet road mirrored the sky in the comparisons)
				}
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
				natives::Invoke<void>(kSetRain, -1.0f); // (the weather's own rain again)
				g_camHandle = 0;
			}
		}
		// (after the camera, which sets noon and a clear sky)
		if (g_hour >= 0)
		{
			game::natives::Invoke<void>(kSetClockTime, g_hour, g_minute, 0);
			game::natives::Invoke<void>(kPauseClock, g_clockRuns ? 0 : 1);
			log::Info("remote: clock at {:02}:{:02}{}", g_hour, g_minute, g_clockRuns ? ", running" : "");
			g_hour = -1;
		}
		if (g_date[0])
		{
			game::natives::Invoke<void>(kSetClockDate, g_date[0], g_date[1] - 1, g_date[2]);
			log::Info("remote: date {}-{:02}-{:02}", g_date[2], g_date[1], g_date[0]);
			g_date[0] = 0;
		}
		if (!g_weather.empty())
		{
			game::natives::Invoke<void>(kSetWeatherTypeNowPersist, g_weather.c_str());
			log::Info("remote: weather {}", g_weather);
			g_weather.clear();
		}
	}
}
