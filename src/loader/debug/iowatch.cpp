#include "iowatch.hpp"

#include <Windows.h>
#include <winternl.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cwctype>
#include <format>
#include <map>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

#include <MinHook.h>

namespace loader::debug::iowatch
{
	namespace
	{
		using NtCreateFileFn = NTSTATUS(NTAPI*)(PHANDLE, ACCESS_MASK, POBJECT_ATTRIBUTES, PIO_STATUS_BLOCK, PLARGE_INTEGER, ULONG, ULONG, ULONG, ULONG,
		    PVOID, ULONG);
		using NtOpenFileFn = NTSTATUS(NTAPI*)(PHANDLE, ACCESS_MASK, POBJECT_ATTRIBUTES, PIO_STATUS_BLOCK, ULONG, ULONG);
		using NtReadFileFn = NTSTATUS(NTAPI*)(HANDLE, HANDLE, PIO_APC_ROUTINE, PVOID, PIO_STATUS_BLOCK, PVOID, ULONG, PLARGE_INTEGER, PULONG);
		using NtCloseFn = NTSTATUS(NTAPI*)(HANDLE);
		NtCreateFileFn g_create = nullptr;
		NtOpenFileFn g_open = nullptr;
		NtReadFileFn g_read = nullptr;
		NtCloseFn g_close = nullptr;
		void* g_targets[4]{};

		std::atomic<bool> g_on = false;
		DWORD g_gameThread = 0;
		std::wstring g_filter; // (lower case)

		struct File
		{
			unsigned opens = 0;
			ULONG options = 0, access = 0, share = 0; // (every open's, or'd)
			uint64_t reads = 0, bytes = 0, gameReads = 0, pending = 0;
			uint64_t sizes[6]{}; // (up to 4 KB, 16 KB, 64 KB, 256 KB, 1 MB, more)
			double inCall = 0, gameInCall = 0, longest = 0; // ms
		};
		std::mutex g_mutex;
		std::map<std::wstring, File> g_files;
		std::unordered_map<HANDLE, std::wstring> g_handles;

		void Opened(HANDLE handle, POBJECT_ATTRIBUTES attributes, ACCESS_MASK access, ULONG share, ULONG options)
		{
			if (!attributes || !attributes->ObjectName || !attributes->ObjectName->Buffer)
				return;
			const std::wstring name(attributes->ObjectName->Buffer, attributes->ObjectName->Length / sizeof(wchar_t));
			std::wstring lower = name;
			for (auto& c : lower)
				c = static_cast<wchar_t>(std::towlower(c));
			if (lower.find(g_filter) == std::wstring::npos)
				return;
			std::lock_guard lock(g_mutex);
			File& f = g_files[name];
			++f.opens;
			f.options |= options, f.access |= access, f.share |= share;
			g_handles[handle] = name;
		}

		NTSTATUS NTAPI HookCreate(PHANDLE handle, ACCESS_MASK access, POBJECT_ATTRIBUTES attributes, PIO_STATUS_BLOCK status, PLARGE_INTEGER size,
		    ULONG fileAttributes, ULONG share, ULONG disposition, ULONG options, PVOID ea, ULONG eaLength)
		{
			const NTSTATUS result = g_create(handle, access, attributes, status, size, fileAttributes, share, disposition, options, ea, eaLength);
			if (g_on && result >= 0 && handle)
				Opened(*handle, attributes, access, share, options);
			return result;
		}

		NTSTATUS NTAPI HookOpen(PHANDLE handle, ACCESS_MASK access, POBJECT_ATTRIBUTES attributes, PIO_STATUS_BLOCK status, ULONG share, ULONG options)
		{
			const NTSTATUS result = g_open(handle, access, attributes, status, share, options);
			if (g_on && result >= 0 && handle)
				Opened(*handle, attributes, access, share, options);
			return result;
		}

		NTSTATUS NTAPI HookRead(HANDLE handle, HANDLE event, PIO_APC_ROUTINE apc, PVOID context, PIO_STATUS_BLOCK status, PVOID buffer, ULONG length,
		    PLARGE_INTEGER offset, PULONG key)
		{
			if (!g_on)
				return g_read(handle, event, apc, context, status, buffer, length, offset, key);
			const auto t0 = std::chrono::steady_clock::now();
			const NTSTATUS result = g_read(handle, event, apc, context, status, buffer, length, offset, key);
			const double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
			std::lock_guard lock(g_mutex);
			const auto it = g_handles.find(handle);
			if (it == g_handles.end())
				return result;
			File& f = g_files[it->second];
			++f.reads;
			f.bytes += length;
			const ULONG bounds[5] = {4096, 16384, 65536, 262144, 1048576};
			size_t bucket = 0;
			while (bucket < 5 && length > bounds[bucket])
				++bucket;
			++f.sizes[bucket];
			f.inCall += ms;
			f.longest = std::max(f.longest, ms);
			f.pending += result == static_cast<NTSTATUS>(STATUS_PENDING);
			if (GetCurrentThreadId() == g_gameThread)
				++f.gameReads, f.gameInCall += ms;
			return result;
		}

		NTSTATUS NTAPI HookClose(HANDLE handle)
		{
			if (g_on)
			{
				std::lock_guard lock(g_mutex);
				g_handles.erase(handle);
			}
			return g_close(handle);
		}

		std::string Utf8(const std::wstring& text)
		{
			const int n = WideCharToMultiByte(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), nullptr, 0, nullptr, nullptr);
			std::string out(static_cast<size_t>(n), '\0');
			WideCharToMultiByte(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), out.data(), n, nullptr, nullptr);
			return out;
		}
	}

	bool Start(const std::wstring& filter, unsigned long thread)
	{
		if (!g_targets[0])
		{
			HMODULE ntdll = GetModuleHandleW(L"ntdll.dll");
			const char* names[4] = {"NtCreateFile", "NtOpenFile", "NtReadFile", "NtClose"};
			void* hooks[4] = {reinterpret_cast<void*>(&HookCreate), reinterpret_cast<void*>(&HookOpen), reinterpret_cast<void*>(&HookRead),
			    reinterpret_cast<void*>(&HookClose)};
			void** originals[4] = {reinterpret_cast<void**>(&g_create), reinterpret_cast<void**>(&g_open), reinterpret_cast<void**>(&g_read),
			    reinterpret_cast<void**>(&g_close)};
			for (int k = 0; k < 4; ++k)
			{
				g_targets[k] = reinterpret_cast<void*>(GetProcAddress(ntdll, names[k]));
				if (!g_targets[k] || MH_CreateHook(g_targets[k], hooks[k], originals[k]) != MH_OK)
					return false;
			}
		}
		{
			std::lock_guard lock(g_mutex);
			g_files.clear();
			g_handles.clear();
			g_filter = filter;
			for (auto& c : g_filter)
				c = static_cast<wchar_t>(std::towlower(c));
		}
		g_gameThread = thread;
		for (void* target : g_targets)
			if (MH_EnableHook(target) != MH_OK)
				return false;
		g_on = true;
		return true;
	}

	std::string Stop()
	{
		g_on = false;
		for (void* target : g_targets)
			if (target)
				MH_DisableHook(target);
		std::lock_guard lock(g_mutex);
		std::string out = std::format("ok {} file(s)", g_files.size());
		for (const auto& [name, f] : g_files)
		{
			// FILE_NO_INTERMEDIATE_BUFFERING 0x8: past the file cache; FILE_SYNCHRONOUS_IO_(NON)ALERT 0x10 / 0x20: synchronous, else
			// overlapped; FILE_SEQUENTIAL_ONLY 0x4, FILE_RANDOM_ACCESS 0x800
			out += std::format(" | {}: opened {}x, options {:#x} ({}, {}), access {:#x}; {} reads ({} on the game thread, {} went on after the call), {} MB, "
			                   "sizes <=4K {} <=16K {} <=64K {} <=256K {} <=1M {} more {}; in the call {:.0f} ms (game thread {:.0f}), longest {:.1f}",
			    Utf8(name), f.opens, f.options, f.options & 0x8 ? "past the cache" : "through the cache", f.options & 0x30 ? "synchronous" : "overlapped",
			    f.access, f.reads, f.gameReads, f.pending, f.bytes >> 20, f.sizes[0], f.sizes[1], f.sizes[2], f.sizes[3], f.sizes[4], f.sizes[5], f.inCall,
			    f.gameInCall, f.longest);
		}
		return out;
	}
}
