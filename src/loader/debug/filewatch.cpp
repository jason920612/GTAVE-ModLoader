#include "filewatch.hpp"

#include <Windows.h>
#include <winternl.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <format>
#include <map>
#include <mutex>
#include <string>
#include <vector>

#include <MinHook.h>

namespace loader::debug::filewatch
{
	namespace
	{
		using NtCreateFileFn = NTSTATUS(NTAPI*)(PHANDLE, ACCESS_MASK, POBJECT_ATTRIBUTES, PIO_STATUS_BLOCK, PLARGE_INTEGER, ULONG, ULONG, ULONG, ULONG,
		    PVOID, ULONG);
		using NtQueryFullAttributesFileFn = NTSTATUS(NTAPI*)(POBJECT_ATTRIBUTES, PVOID);
		NtCreateFileFn g_createFile = nullptr;
		NtQueryFullAttributesFileFn g_queryAttributes = nullptr;
		void *g_createTarget = nullptr, *g_queryTarget = nullptr;

		std::atomic<DWORD> g_thread = 0; // (0: off)
		struct Seen
		{
			unsigned count = 0;
			double total = 0, longest = 0; // ms
		};
		std::mutex g_mutex;
		std::map<std::wstring, Seen> g_seen; // (by what was asked and the file's name)

		void Note(const wchar_t* what, POBJECT_ATTRIBUTES attributes, std::chrono::steady_clock::time_point since)
		{
			const double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - since).count();
			std::wstring name = what;
			if (attributes && attributes->ObjectName && attributes->ObjectName->Buffer)
				name.append(attributes->ObjectName->Buffer, attributes->ObjectName->Length / sizeof(wchar_t));
			std::lock_guard lock(g_mutex);
			Seen& s = g_seen[name];
			++s.count;
			s.total += ms;
			s.longest = std::max(s.longest, ms);
		}

		NTSTATUS NTAPI HookCreateFile(PHANDLE handle, ACCESS_MASK access, POBJECT_ATTRIBUTES attributes, PIO_STATUS_BLOCK status, PLARGE_INTEGER size,
		    ULONG fileAttributes, ULONG share, ULONG disposition, ULONG options, PVOID ea, ULONG eaLength)
		{
			if (GetCurrentThreadId() != g_thread.load(std::memory_order_relaxed))
				return g_createFile(handle, access, attributes, status, size, fileAttributes, share, disposition, options, ea, eaLength);
			const auto since = std::chrono::steady_clock::now();
			const NTSTATUS result = g_createFile(handle, access, attributes, status, size, fileAttributes, share, disposition, options, ea, eaLength);
			Note(L"open ", attributes, since);
			return result;
		}

		NTSTATUS NTAPI HookQueryAttributes(POBJECT_ATTRIBUTES attributes, PVOID information)
		{
			if (GetCurrentThreadId() != g_thread.load(std::memory_order_relaxed))
				return g_queryAttributes(attributes, information);
			const auto since = std::chrono::steady_clock::now();
			const NTSTATUS result = g_queryAttributes(attributes, information);
			Note(L"attributes ", attributes, since);
			return result;
		}

		std::string Utf8(const std::wstring& text)
		{
			const int n = WideCharToMultiByte(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), nullptr, 0, nullptr, nullptr);
			std::string out(static_cast<size_t>(n), '\0');
			WideCharToMultiByte(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), out.data(), n, nullptr, nullptr);
			return out;
		}
	}

	bool Start(unsigned long thread)
	{
		if (!g_createTarget)
		{
			HMODULE ntdll = GetModuleHandleW(L"ntdll.dll");
			g_createTarget = reinterpret_cast<void*>(GetProcAddress(ntdll, "NtCreateFile"));
			g_queryTarget = reinterpret_cast<void*>(GetProcAddress(ntdll, "NtQueryFullAttributesFile"));
			if (!g_createTarget || !g_queryTarget ||
			    MH_CreateHook(g_createTarget, reinterpret_cast<void*>(&HookCreateFile), reinterpret_cast<void**>(&g_createFile)) != MH_OK ||
			    MH_CreateHook(g_queryTarget, reinterpret_cast<void*>(&HookQueryAttributes), reinterpret_cast<void**>(&g_queryAttributes)) != MH_OK)
				return false;
		}
		{
			std::lock_guard lock(g_mutex);
			g_seen.clear();
		}
		g_thread = thread;
		return MH_EnableHook(g_createTarget) == MH_OK && MH_EnableHook(g_queryTarget) == MH_OK;
	}

	std::string Stop()
	{
		g_thread = 0;
		if (g_createTarget)
			MH_DisableHook(g_createTarget), MH_DisableHook(g_queryTarget);
		std::vector<std::pair<std::wstring, Seen>> all;
		{
			std::lock_guard lock(g_mutex);
			all.assign(g_seen.begin(), g_seen.end());
		}
		std::sort(all.begin(), all.end(), [](const auto& a, const auto& b) { return a.second.total > b.second.total; });
		std::string out = std::format("ok {} file(s)", all.size());
		for (size_t i = 0; i < all.size() && i < 25; ++i)
			out += std::format(" | {} x{} {:.1f} ms in all, longest {:.1f} ms", Utf8(all[i].first), all[i].second.count, all[i].second.total,
			    all[i].second.longest);
		return out;
	}
}
