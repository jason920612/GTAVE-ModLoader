#include "watchdog.hpp"

#include <Windows.h>
#include <DbgHelp.h>

#include <algorithm>
#include <atomic>
#include <ctime>
#include <filesystem>
#include <format>
#include <fstream>
#include <sstream>
#include <thread>
#include <vector>

#include <MinHook.h>

#include "../log.hpp"
#include "../paths.hpp"
#include "../pattern.hpp"

namespace loader::debug::watchdog
{
	namespace
	{
		uintptr_t g_base = 0, g_end = 0;
		bool g_full = false;                  // (debug_arrayfault.txt "dump full": the whole memory at once)
		volatile long* g_hangPause = nullptr; // (the game's hang watchdog: held off while a fault is held, as dlcpacks.cpp does)

		// The fault held: its thread waits in Hold for the pipe's word (1 let it go on, 2 step over the int3).
		std::atomic<bool> g_holding = false;
		std::atomic<int> g_word = 0;
		EXCEPTION_POINTERS* g_held = nullptr;
		DWORD g_heldThread = 0;
		std::string g_what;
		const EXCEPTION_RECORD* g_letGo = nullptr; // (a fault let go: not held again on its way to the unhandled filter)
		constexpr const wchar_t* kTitle = L"GTA V Mod Loader 看門狗";

		// The unhandled exception filter the game set: ours stays first, the game's runs after it.
		LPTOP_LEVEL_EXCEPTION_FILTER g_next = nullptr;
		using SetFilterFn = LPTOP_LEVEL_EXCEPTION_FILTER(WINAPI*)(LPTOP_LEVEL_EXCEPTION_FILTER);
		SetFilterFn g_setFilter = nullptr;

		using WriteDumpFn = BOOL(WINAPI*)(HANDLE, DWORD, HANDLE, MINIDUMP_TYPE, PMINIDUMP_EXCEPTION_INFORMATION,
		    PMINIDUMP_USER_STREAM_INFORMATION, PMINIDUMP_CALLBACK_INFORMATION);

		std::string Where(uintptr_t at)
		{
			HMODULE module = nullptr;
			wchar_t path[MAX_PATH] = L"?";
			if (GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT, reinterpret_cast<LPCWSTR>(at),
			        &module))
				GetModuleFileNameW(module, path, MAX_PATH);
			return std::format("{}+{:#x}", std::filesystem::path(path).stem().string(), at - reinterpret_cast<uintptr_t>(module));
		}

		// The held moment in ModLoader\crash (the newest four kept: a whole memory is gigabytes). Written from a thread of
		// its own: MiniDumpWriteDump walks the held thread's stack from outside it.
		std::string WriteDump(bool full)
		{
			HMODULE dbghelp = LoadLibraryW(L"dbghelp.dll");
			const auto write = dbghelp ? reinterpret_cast<WriteDumpFn>(GetProcAddress(dbghelp, "MiniDumpWriteDump")) : nullptr;
			if (!write || !g_held)
				return "none written (dbghelp.dll)";
			const auto dir = paths::Get().root / L"crash";
			std::error_code ec;
			std::filesystem::create_directories(dir, ec);
			std::vector<std::filesystem::path> dumps;
			for (const auto& e : std::filesystem::directory_iterator(dir, ec))
				if (e.path().extension() == L".dmp")
					dumps.push_back(e.path());
			std::sort(dumps.begin(), dumps.end());
			while (dumps.size() >= 4)
			{
				std::filesystem::remove(dumps.front(), ec);
				dumps.erase(dumps.begin());
			}
			const auto now = std::time(nullptr);
			std::tm tm{};
			localtime_s(&tm, &now);
			const auto file = dir / std::format(L"crash-{:04}{:02}{:02}-{:02}{:02}{:02}{}.dmp", tm.tm_year + 1900, tm.tm_mon + 1, tm.tm_mday, tm.tm_hour,
			                             tm.tm_min, tm.tm_sec, full ? L"-full" : L"");
			std::string result;
			std::thread([&] {
				const HANDLE h = CreateFileW(file.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
				if (h == INVALID_HANDLE_VALUE)
				{
					result = "could not open " + file.string();
					return;
				}
				MINIDUMP_EXCEPTION_INFORMATION info{g_heldThread, g_held, FALSE};
				const auto type = static_cast<MINIDUMP_TYPE>(
				    (full ? MiniDumpWithFullMemory : MiniDumpWithDataSegs | MiniDumpWithIndirectlyReferencedMemory) | MiniDumpWithFullMemoryInfo |
				    MiniDumpWithHandleData | MiniDumpWithThreadInfo | MiniDumpWithUnloadedModules);
				const BOOL ok = write(GetCurrentProcess(), GetCurrentProcessId(), h, type, &info, nullptr, nullptr);
				const DWORD error = GetLastError();
				LARGE_INTEGER size{};
				GetFileSizeEx(h, &size);
				CloseHandle(h);
				result = ok ? std::format("{} ({} MB)", file.string(), size.QuadPart >> 20) : std::format("could not write {} ({:#x})", file.string(), error);
			}).join();
			return result;
		}

		LONG Hold(EXCEPTION_POINTERS* e, const char* how)
		{
			// (a second fault while one is held - another thread's: it waits as well)
			if (g_holding.exchange(true))
				for (;;)
					Sleep(1000);
			if (g_hangPause)
				_InterlockedIncrement(g_hangPause);
			g_held = e;
			g_heldThread = GetCurrentThreadId();
			g_word = 0;
			g_what = std::format("{} at {} (exception {:#x}, thread {})", how, Where(reinterpret_cast<uintptr_t>(e->ExceptionRecord->ExceptionAddress)),
			    e->ExceptionRecord->ExceptionCode, g_heldThread);
			log::Error("watchdog: {}; the game is held. The pipe: crash [status | dump full | resume | skip]", g_what);
			log::Warn("watchdog: the moment: {}", WriteDump(false));
			MessageBeep(MB_ICONHAND);
			std::thread([] {
				const int answer = MessageBoxW(nullptr,
				    L"遊戲發生了會讓它當掉的錯誤。模組載入器把遊戲停在那一刻，當下的記憶體存在 ModLoader\\crash。\n\n按「確定」讓遊戲結束。", kTitle,
				    MB_OK | MB_ICONERROR | MB_TOPMOST | MB_SETFOREGROUND);
				int none = 0;
				if (answer == IDOK)
					g_word.compare_exchange_strong(none, 1);
			}).detach();
			// (the whole memory too, a minute or so for the game's 9 GB: let go only once it is written)
			if (g_full)
				log::Warn("watchdog: the whole memory: {}", WriteDump(true));
			while (g_word == 0)
				Sleep(100);
			const int word = g_word;
			if (const HWND box = FindWindowW(nullptr, kTitle))
				PostMessageW(box, WM_CLOSE, 0, 0);
			g_held = nullptr;
			if (g_hangPause)
				_InterlockedDecrement(g_hangPause);
			g_holding = false;
			if (word == 2 && e->ExceptionRecord->ExceptionCode == EXCEPTION_BREAKPOINT)
			{
				++e->ContextRecord->Rip;
				log::Warn("watchdog: stepped over the int3; the game goes on");
				return EXCEPTION_CONTINUE_EXECUTION;
			}
			g_letGo = e->ExceptionRecord;
			log::Warn("watchdog: let go; the game's own handling goes on");
			return EXCEPTION_CONTINUE_SEARCH;
		}

		// After the other handlers (arrayfault.cpp's logs the fault and its calls first).
		LONG CALLBACK OnException(EXCEPTION_POINTERS* e)
		{
			const auto* record = e->ExceptionRecord;
			const auto at = reinterpret_cast<uintptr_t>(record->ExceptionAddress);
			// the game's own fatal checks: an int3 in its code
			if (record->ExceptionCode == EXCEPTION_BREAKPOINT && at >= g_base && at < g_end && *reinterpret_cast<const uint8_t*>(at) == 0xCC &&
			    record != g_letGo && !IsDebuggerPresent())
				return Hold(e, "the game's own check (int3)");
			return EXCEPTION_CONTINUE_SEARCH;
		}

		LONG WINAPI OnUnhandled(EXCEPTION_POINTERS* e)
		{
			if (e->ExceptionRecord != g_letGo && Hold(e, "an exception nothing handled") == EXCEPTION_CONTINUE_EXECUTION)
				return EXCEPTION_CONTINUE_EXECUTION;
			return g_next ? g_next(e) : EXCEPTION_CONTINUE_SEARCH;
		}

		LPTOP_LEVEL_EXCEPTION_FILTER WINAPI SetFilter(LPTOP_LEVEL_EXCEPTION_FILTER filter)
		{
			const auto was = g_next;
			g_next = filter;
			return was;
		}
	}

	void Install()
	{
		std::error_code ec;
		const auto config = paths::Get().root / L"debug_arrayfault.txt";
		if (!std::filesystem::exists(config, ec))
			return;
		std::ifstream in(config);
		for (std::string line; std::getline(in, line);)
			if (line.starts_with("dump full"))
				g_full = true;
		const auto module = pattern::Module::Main();
		g_base = module.base;
		g_end = module.base + module.size;
		if (const auto at = pattern::Find(module.text, pattern::Pattern::Parse("8B 05 ? ? ? ? 85 C0 75 E2 48 8B 05 ? ? ? ? 48 39 C6")))
			g_hangPause = reinterpret_cast<volatile long*>(pattern::Rip(*at + 2));
		AddVectoredExceptionHandler(0, OnException);
		if (const HMODULE kernel = GetModuleHandleW(L"kernelbase.dll"))
			if (void* set = GetProcAddress(kernel, "SetUnhandledExceptionFilter"))
				if (MH_CreateHook(set, reinterpret_cast<void*>(&SetFilter), reinterpret_cast<void**>(&g_setFilter)) == MH_OK && MH_EnableHook(set) == MH_OK)
					g_next = g_setFilter(&OnUnhandled);
		log::Info("watchdog: fatal errors held (the game's int3 checks{}; its hang watchdog {}), the moment written to ModLoader\\crash{}",
		    g_setFilter ? ", exceptions nothing handles" : "", g_hangPause ? "held off meanwhile" : "not found", g_full ? " (the whole memory)" : "");
	}

	std::string Command(const std::string& args)
	{
		std::istringstream in(args);
		std::string word, more;
		in >> word >> more;
		if (word.empty() || word == "status")
			return g_holding ? "ok held: " + g_what : std::string("ok not held");
		if (word == "dump")
			return g_holding ? "ok " + WriteDump(more == "full") : std::string("err nothing held");
		if (word == "resume" || word == "skip")
		{
			if (!g_holding)
				return "err nothing held";
			g_word = word == "skip" ? 2 : 1;
			return "ok";
		}
		return "err crash [status] | dump [full] | resume | skip | test";
	}

	std::string Test()
	{
		// (an int3 of the game's code with a return after it: held there, then "crash skip" comes back here)
		const auto module = pattern::Module::Main();
		const auto at = pattern::Find(module.text, pattern::Pattern::Parse("CC C3"));
		if (!at)
			return "no int3 with a return after it in the game's code";
		reinterpret_cast<void (*)()>(*at)();
		return "went on past the int3";
	}
}
