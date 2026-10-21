#include "remote.hpp"

#include <Windows.h>
#include <audiopolicy.h>
#include <mmdeviceapi.h>

#include <algorithm>
#include <atomic>
#include <cctype>
#include <cstring>
#include <map>
#include <mutex>
#include <sstream>
#include <string>
#include <thread>

#include "../config.hpp"
#include "../game/natives.hpp"
#include "../log.hpp"
#include "../state.hpp"
#include "../ui/overlay.hpp"

namespace loader::debug::remote
{
	namespace
	{
		constexpr uint64_t kSetControlValueNextFrame = 0xE8A25867FBA3B05E;
		constexpr wchar_t kPipeName[] = L"\\\\.\\pipe\\gtave-modloader";

		struct Held
		{
			float value = 1.0f;
			ULONGLONG until = 0;
		};
		std::mutex g_mutex;
		std::map<int, Held> g_held; // control -> how long

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

		// The game's own audio session (the volume mixer's entry), so a test running beside the person is silent.
		std::string Mute(bool on)
		{
			const HRESULT co = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
			std::string result = "err no audio session";
			IMMDeviceEnumerator* devices = nullptr;
			IMMDevice* device = nullptr;
			IAudioSessionManager* sessions = nullptr;
			ISimpleAudioVolume* volume = nullptr;
			if (SUCCEEDED(CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL, IID_PPV_ARGS(&devices))) &&
			    SUCCEEDED(devices->GetDefaultAudioEndpoint(eRender, eConsole, &device)) &&
			    SUCCEEDED(device->Activate(__uuidof(IAudioSessionManager), CLSCTX_ALL, nullptr, reinterpret_cast<void**>(&sessions))) &&
			    SUCCEEDED(sessions->GetSimpleAudioVolume(nullptr, FALSE, &volume)) && SUCCEEDED(volume->SetMute(on, nullptr)))
				result = "ok";
			if (volume)
				volume->Release();
			if (sessions)
				sessions->Release();
			if (device)
				device->Release();
			if (devices)
				devices->Release();
			if (SUCCEEDED(co))
				CoUninitialize();
			return result;
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
			return;
		std::thread(Serve).detach();
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
	}
}
