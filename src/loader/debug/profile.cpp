#include "profile.hpp"

#include <Windows.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <format>
#include <map>
#include <memory>
#include <thread>
#include <vector>

namespace loader::debug::profile
{
	namespace
	{
		struct Samples
		{
			static constexpr int kMax = 20000;
			uintptr_t from = 0, to = 0;
			std::atomic<bool> running = false;
			int taken = 0;
			std::array<uint32_t, kMax> leaf{}, site{};
		};

		void SampleThread(Samples* p, HANDLE thread)
		{
			const auto base = reinterpret_cast<uintptr_t>(GetModuleHandleW(nullptr));
			while (p->running && p->taken < Samples::kMax)
			{
				if (SuspendThread(thread) != static_cast<DWORD>(-1))
				{
					CONTEXT c{};
					c.ContextFlags = CONTEXT_FULL;
					if (GetThreadContext(thread, &c))
					{
						uint32_t leaf = 0, site = 0;
						for (int i = 0; i < 64 && c.Rip; ++i)
						{
							DWORD64 image = 0;
							RUNTIME_FUNCTION* f = RtlLookupFunctionEntry(c.Rip, &image, nullptr);
							if (image == base)
							{
								if (!leaf)
									leaf = f ? f->BeginAddress : static_cast<uint32_t>(c.Rip - base);
								if (i && c.Rip >= base + p->from && c.Rip < base + p->to)
								{
									site = static_cast<uint32_t>(c.Rip - base);
									break;
								}
							}
							if (f)
							{
								void* data = nullptr;
								DWORD64 frame = 0;
								RtlVirtualUnwind(UNW_FLAG_NHANDLER, image, c.Rip, f, &c, &data, &frame, nullptr);
							}
							else
							{
								c.Rip = *reinterpret_cast<const DWORD64*>(c.Rsp);
								c.Rsp += 8;
							}
						}
						p->leaf[p->taken] = leaf;
						p->site[p->taken] = site;
						++p->taken;
					}
					ResumeThread(thread);
				}
				const auto until = std::chrono::steady_clock::now() + std::chrono::microseconds(1000);
				while (std::chrono::steady_clock::now() < until)
					std::this_thread::yield();
			}
		}
	}

	std::string Profiled(uintptr_t from, uintptr_t to, const std::function<void()>& run)
	{
		auto p = std::make_unique<Samples>();
		p->from = from, p->to = to;
		HANDLE self = OpenThread(THREAD_SUSPEND_RESUME | THREAD_GET_CONTEXT | THREAD_QUERY_INFORMATION, FALSE, GetCurrentThreadId());
		p->running = true;
		std::thread sampler(SampleThread, p.get(), self);
		run();
		p->running = false;
		sampler.join();
		CloseHandle(self);
		std::map<uint32_t, int> sites, leaves;
		for (int i = 0; i < p->taken; ++i)
			++sites[p->site[i]], ++leaves[p->leaf[i]];
		const auto top = [](const std::map<uint32_t, int>& m, size_t n) {
			std::vector<std::pair<int, uint32_t>> v;
			for (const auto& [k, c] : m)
				v.push_back({c, k});
			std::sort(v.rbegin(), v.rend());
			std::string out;
			for (size_t i = 0; i < std::min(n, v.size()); ++i)
				out += std::format(" {:#x}:{}", v[i].second, v[i].first);
			return out;
		};
		return std::format("{} samples; sites{}; functions{}", p->taken, top(sites, 14), top(leaves, 14));
	}

	namespace
	{
		struct Trace
		{
			struct Sample
			{
				std::chrono::steady_clock::time_point at;
				uintptr_t leaf = 0, leafBase = 0; // (the innermost function's start and its module)
				std::array<uint32_t, 6> through{};
			};
			static constexpr size_t kMax = 600000; // (ten minutes)
			std::vector<Sample> samples = std::vector<Sample>(kMax);
			std::atomic<size_t> taken = 0;
			std::atomic<bool> running = false;
			HANDLE thread = nullptr;
			std::thread sampler;
		};
		std::unique_ptr<Trace> g_trace;

		void TraceThread(Trace* t)
		{
			const auto base = reinterpret_cast<uintptr_t>(GetModuleHandleW(nullptr));
			while (t->running)
			{
				if (SuspendThread(t->thread) != static_cast<DWORD>(-1))
				{
					CONTEXT c{};
					c.ContextFlags = CONTEXT_FULL;
					if (GetThreadContext(t->thread, &c))
					{
						Trace::Sample& s = t->samples[t->taken % Trace::kMax];
						s = {};
						s.at = std::chrono::steady_clock::now();
						size_t n = 0;
						for (int i = 0; i < 64 && c.Rip && n < s.through.size(); ++i)
						{
							DWORD64 image = 0;
							RUNTIME_FUNCTION* f = RtlLookupFunctionEntry(c.Rip, &image, nullptr);
							if (i == 0)
								s.leaf = f ? image + f->BeginAddress : c.Rip, s.leafBase = image;
							else if (image == base && f)
								s.through[n++] = f->BeginAddress;
							if (f)
							{
								void* data = nullptr;
								DWORD64 frame = 0;
								RtlVirtualUnwind(UNW_FLAG_NHANDLER, image, c.Rip, f, &c, &data, &frame, nullptr);
							}
							else
							{
								c.Rip = *reinterpret_cast<const DWORD64*>(c.Rsp);
								c.Rsp += 8;
							}
						}
						++t->taken;
					}
					ResumeThread(t->thread);
				}
				const auto until = std::chrono::steady_clock::now() + std::chrono::microseconds(1000);
				while (std::chrono::steady_clock::now() < until)
					std::this_thread::yield();
			}
		}
	}

	void Start()
	{
		Stop();
		g_trace = std::make_unique<Trace>();
		g_trace->thread = OpenThread(THREAD_SUSPEND_RESUME | THREAD_GET_CONTEXT | THREAD_QUERY_INFORMATION, FALSE, GetCurrentThreadId());
		g_trace->running = true;
		g_trace->sampler = std::thread(TraceThread, g_trace.get());
	}

	void Stop()
	{
		if (!g_trace || !g_trace->running)
			return;
		g_trace->running = false;
		g_trace->sampler.join();
		CloseHandle(g_trace->thread);
	}

	std::string Between(std::chrono::steady_clock::time_point from, std::chrono::steady_clock::time_point to)
	{
		if (!g_trace)
			return "not sampled";
		const auto game = reinterpret_cast<uintptr_t>(GetModuleHandleW(nullptr));
		std::map<std::string, int> leaves, chains;
		int n = 0;
		const size_t taken = g_trace->taken, first = taken > Trace::kMax ? taken - Trace::kMax : 0;
		for (size_t i = first; i < taken; ++i)
		{
			const Trace::Sample& s = g_trace->samples[i % Trace::kMax];
			if (s.at < from || s.at > to)
				continue;
			++n;
			std::string where;
			if (s.leafBase == game)
				where = std::format("{:#x}", s.leaf - game);
			else
			{
				wchar_t path[MAX_PATH] = L"?";
				if (s.leafBase)
					GetModuleFileNameW(reinterpret_cast<HMODULE>(s.leafBase), path, MAX_PATH);
				std::wstring_view wide(path);
				if (const size_t slash = wide.find_last_of(L"\\/"); slash != std::wstring_view::npos)
					wide.remove_prefix(slash + 1);
				std::string name;
				for (const wchar_t ch : wide)
					name += static_cast<char>(ch);
				where = std::format("{}+{:#x}", name, s.leaf - s.leafBase);
			}
			++leaves[where];
			std::string chain;
			for (const uint32_t f : s.through)
				if (f)
					chain += std::format("{}{:#x}", chain.empty() ? "" : "<", f);
			++chains[chain.empty() ? "-" : chain];
		}
		const auto top = [](const std::map<std::string, int>& m, size_t count) {
			std::vector<std::pair<int, std::string>> v;
			for (const auto& [k, c] : m)
				v.push_back({c, k});
			std::sort(v.rbegin(), v.rend());
			std::string out;
			for (size_t i = 0; i < std::min(count, v.size()); ++i)
				out += std::format(" {}:{}", v[i].second, v[i].first);
			return out;
		};
		return std::format("{} samples; functions{}; through{}", n, top(leaves, 8), top(chains, 5));
	}
}
