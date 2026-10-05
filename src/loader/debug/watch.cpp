#include "watch.hpp"

#include <Windows.h>
#include <TlHelp32.h>

#include <atomic>
#include <filesystem>
#include <thread>
#include <format>
#include <string>

#include "../log.hpp"
#include "../paths.hpp"
#include "../pattern.hpp"

#include <fstream>
#include <sstream>

namespace loader::debug
{
	namespace
	{
		constexpr int kMaxHitsPerWatch = 40;

		pattern::Module g_game;
		uintptr_t g_watchAddr[4]{};
		std::atomic_int g_hits[4]{};
		PVOID g_handler = nullptr;

		bool InGameCode(uintptr_t a)
		{
			return a >= g_game.base && a < g_game.base + g_game.size;
		}

		LONG CALLBACK OnException(EXCEPTION_POINTERS* info)
		{
			if (info->ExceptionRecord->ExceptionCode != EXCEPTION_SINGLE_STEP)
				return EXCEPTION_CONTINUE_SEARCH;
			CONTEXT* ctx = info->ContextRecord;
			const DWORD64 dr6 = ctx->Dr6;
			if (!(dr6 & 0xF))
				return EXCEPTION_CONTINUE_SEARCH;
			ctx->Dr6 = 0;

			for (int i = 0; i < 4; ++i)
			{
				if (!(dr6 & (1ull << i)) || g_hits[i]++ >= kMaxHitsPerWatch)
					continue;
				// Return addresses into game code found near the top of the stack.
				std::string stack;
				int found = 0;
				const auto* sp = reinterpret_cast<const uintptr_t*>(ctx->Rsp);
				for (int k = 0; k < 160 && found < 14; ++k)
				{
					uintptr_t v = 0;
					if (ReadProcessMemory(GetCurrentProcess(), sp + k, &v, sizeof(v), nullptr) && InGameCode(v))
					{
						stack += std::format(" {:#x}", v - g_game.base);
						++found;
					}
				}
				uint64_t value = 0;
				ReadProcessMemory(GetCurrentProcess(), reinterpret_cast<void*>(g_watchAddr[i]), &value, sizeof(value), nullptr);
				log::Info("watch[{}] {:#x} <- rip {:#x} (now {:#x}) thread {} | stack:{}", i, g_watchAddr[i],
				    ctx->Rip - g_game.base, value, GetCurrentThreadId(), stack);
			}
			return EXCEPTION_CONTINUE_EXECUTION;
		}

		DWORD64 Dr7For(const std::vector<Watch>& watches)
		{
			DWORD64 dr7 = 0;
			for (size_t i = 0; i < watches.size() && i < 4; ++i)
			{
				const DWORD64 len = watches[i].size == 1 ? 0 : watches[i].size == 2 ? 1 : watches[i].size == 8 ? 2 : 3;
				dr7 |= 1ull << (i * 2);              // local enable
				dr7 |= (watches[i].onRead ? 3ull : 1ull) << (16 + i * 4); // read/write or write
				dr7 |= len << (18 + i * 4);          // length
			}
			return dr7;
		}
	}

	void ArmWriteWatches(const std::vector<Watch>& watches)
	{
		g_game = pattern::Module::Main();
		for (size_t i = 0; i < watches.size() && i < 4; ++i)
		{
			const uintptr_t addr = (watches[i].absolute ? 0 : g_game.base) + watches[i].rva;
			if (g_watchAddr[i] != addr)
				g_hits[i] = 0; // re-arming the same watch keeps its hit budget
			g_watchAddr[i] = addr;
		}
		if (!g_handler)
			g_handler = AddVectoredExceptionHandler(1, OnException);

		const DWORD64 dr7 = Dr7For(watches);
		const DWORD self = GetCurrentThreadId();
		HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0);
		THREADENTRY32 te{sizeof(te)};
		int armed = 0;
		for (BOOL ok = Thread32First(snap, &te); ok; ok = Thread32Next(snap, &te))
		{
			if (te.th32OwnerProcessID != GetCurrentProcessId() || te.th32ThreadID == self)
				continue;
			HANDLE t = OpenThread(THREAD_GET_CONTEXT | THREAD_SET_CONTEXT | THREAD_SUSPEND_RESUME, FALSE, te.th32ThreadID);
			if (!t)
				continue;
			SuspendThread(t);
			CONTEXT ctx{};
			ctx.ContextFlags = CONTEXT_DEBUG_REGISTERS;
			if (GetThreadContext(t, &ctx))
			{
				ctx.Dr0 = g_watchAddr[0];
				ctx.Dr1 = g_watchAddr[1];
				ctx.Dr2 = g_watchAddr[2];
				ctx.Dr3 = g_watchAddr[3];
				ctx.Dr7 = dr7;
				armed += SetThreadContext(t, &ctx) ? 1 : 0;
			}
			ResumeThread(t);
			CloseHandle(t);
		}
		CloseHandle(snap);
		static int lastArmed = -1;
		if (armed != lastArmed)
			log::Info("debug: write watches armed on {} thread(s)", armed);
		lastArmed = armed;
	}

	void PollWatchFile()
	{
		static std::filesystem::file_time_type lastWrite{};
		const auto file = paths::Get().root / L"debug_watch.txt";
		std::error_code ec;
		const auto time = std::filesystem::last_write_time(file, ec);
		if (ec || time == lastWrite)
			return;
		lastWrite = time;
		std::vector<Watch> watches;
		std::ifstream in(file);
		std::string line;
		while (std::getline(in, line) && watches.size() < 4)
		{
			std::istringstream ls(line);
			std::string addr, mode;
			int size = 0;
			if (!(ls >> addr >> size))
				continue;
			ls >> mode;
			watches.push_back({std::stoull(addr, nullptr, 16), static_cast<uint8_t>(size), mode == "r", true});
		}
		log::Info("debug: arming {} watch(es) from debug_watch.txt", watches.size());
		std::thread([watches] { ArmWriteWatches(watches); }).detach();
	}
}
