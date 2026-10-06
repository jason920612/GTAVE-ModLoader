#include "hangdump.hpp"

#include <Windows.h>
#include <DbgHelp.h>

#include <atomic>
#include <chrono>
#include <ctime>
#include <filesystem>
#include <format>
#include <thread>

#include "../log.hpp"
#include "../paths.hpp"
#include "../pattern.hpp"

namespace loader::debug::hangdump
{
	namespace
	{
		constexpr int kDumpAfterSeconds = 45; // the watchdog fires after 60
		std::atomic<int> g_stalled = 0;

		using WriteDumpFn = BOOL(WINAPI*)(HANDLE, DWORD, HANDLE, MINIDUMP_TYPE, PMINIDUMP_EXCEPTION_INFORMATION,
		    PMINIDUMP_USER_STREAM_INFORMATION, PMINIDUMP_CALLBACK_INFORMATION);

		bool WriteDump(int stalledSeconds)
		{
			HMODULE dbghelp = LoadLibraryW(L"dbghelp.dll");
			auto write = dbghelp ? reinterpret_cast<WriteDumpFn>(GetProcAddress(dbghelp, "MiniDumpWriteDump")) : nullptr;
			if (!write)
				return false;
			const auto dir = paths::Get().root / L"hangdumps";
			std::error_code ec;
			std::filesystem::create_directories(dir, ec);
			const auto now = std::time(nullptr);
			std::tm tm{};
			localtime_s(&tm, &now);
			const auto file = dir / std::format(L"hang-{:04}{:02}{:02}-{:02}{:02}{:02}.dmp", tm.tm_year + 1900, tm.tm_mon + 1, tm.tm_mday,
			                             tm.tm_hour, tm.tm_min, tm.tm_sec);
			HANDLE h = CreateFileW(file.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
			if (h == INVALID_HANDLE_VALUE)
				return false;
			// Stacks of every thread and the memory they point at, not the whole heap.
			const auto type = static_cast<MINIDUMP_TYPE>(MiniDumpWithThreadInfo | MiniDumpWithIndirectlyReferencedMemory |
			                                             MiniDumpWithDataSegs | MiniDumpWithHandleData | MiniDumpWithUnloadedModules);
			const BOOL ok = write(GetCurrentProcess(), GetCurrentProcessId(), h, type, nullptr, nullptr, nullptr);
			CloseHandle(h);
			log::Warn("hang: main thread heartbeat stopped for {} s; {} {}", stalledSeconds, ok ? "wrote" : "could not write",
			    file.filename().string());
			return ok != FALSE;
		}

		void Watch(volatile const int* paused, volatile const int64_t* heartbeat)
		{
			int64_t last = *heartbeat;
			int stalled = 0;
			bool dumped = false;
			for (;;)
			{
				Sleep(1000);
				const int64_t now = *heartbeat;
				if (now != last || *paused)
				{
					if (dumped && now != last)
						log::Info("hang: main thread recovered after {} s", stalled);
					last = now;
					stalled = 0;
					g_stalled = 0;
					dumped = false;
					continue;
				}
				g_stalled = ++stalled;
				if (stalled == kDumpAfterSeconds && !dumped)
					dumped = WriteDump(stalled);
			}
		}
	}

	void Start()
	{
		static bool started = false;
		if (started)
			return;
		started = true;
		const auto module = pattern::Module::Main();
		// Watchdog loop: mov eax,[pause counter]; test eax,eax; jnz; mov rax,[heartbeat]; cmp rsi,rax
		const auto at = pattern::Find(module.text, pattern::Pattern::Parse("8B 05 ? ? ? ? 85 C0 75 E2 48 8B 05 ? ? ? ? 48 39 C6"));
		if (!at)
		{
			log::Warn("hang: watchdog not found; no hang dumps");
			return;
		}
		const auto* paused = reinterpret_cast<const int*>(pattern::Rip(*at + 2));
		const auto* heartbeat = reinterpret_cast<const int64_t*>(pattern::Rip(*at + 13));
		std::thread(Watch, paused, heartbeat).detach();
		log::Info("hang: watching the main thread heartbeat (+{:#x})", reinterpret_cast<uintptr_t>(heartbeat) - module.base);
	}

	int StalledSeconds()
	{
		return g_stalled;
	}
}
