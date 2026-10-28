#include "profile.hpp"

#include <Windows.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cstring>
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
				uint8_t thread = 0; // (which of the threads: 0 the game's)
			};
			static constexpr size_t kMax = 1200000; // (ten minutes of the game's thread and another)
			std::vector<Sample> samples = std::vector<Sample>(kMax);
			std::atomic<size_t> taken = 0;
			std::atomic<bool> running = false;
			std::vector<HANDLE> threads;
			std::vector<DWORD> ids;
			std::vector<uintptr_t> stackEnds; // (each thread's stack's end: its TEB's StackBase, 0 not known)
			std::thread sampler;
		};
		std::unique_ptr<Trace> g_trace;

		// a thread's stack's end (its TEB's StackBase, +8: ThreadBasicInformation's TebBaseAddress), 0 if not known
		uintptr_t StackEnd(HANDLE thread)
		{
			struct BasicInformation
			{
				LONG exitStatus;
				void* teb;
				void* clientId[2];
				ULONG_PTR affinity;
				LONG priority, basePriority;
			} info{};
			using QueryFn = LONG(NTAPI*)(HANDLE, int, void*, ULONG, ULONG*);
			static const auto query = reinterpret_cast<QueryFn>(GetProcAddress(GetModuleHandleW(L"ntdll.dll"), "NtQueryInformationThread"));
			if (!query || query(thread, 0, &info, sizeof(info), nullptr) < 0 || !info.teb)
				return 0;
			return *reinterpret_cast<const uintptr_t*>(static_cast<const uint8_t*>(info.teb) + 8);
		}

		// (the stack's top copied while the thread is stopped: nothing that takes a lock is called then - a thread stopped
		// holding the function tables' lock would have held us too; not past the stack's end - the page after it is not there)
		constexpr size_t kStackCopy = 32 * 1024;
		size_t CopyStack(uint8_t* to, uintptr_t from, uintptr_t end)
		{
			const size_t most = end > from ? std::min<size_t>(kStackCopy, end - from) : kStackCopy;
			size_t got = 0;
			__try
			{
				for (; got < most; got += 4096)
				{
					const size_t chunk = std::min<size_t>(4096 - (from + got) % 4096, most - got);
					std::memcpy(to + got, reinterpret_cast<const void*>(from + got), chunk);
					if (chunk != 4096)
						return got + chunk;
				}
			}
			__except (EXCEPTION_EXECUTE_HANDLER)
			{
			}
			return got;
		}

		// the stack walked on the copy (the stack pointer, and the frame pointer when it points into the copied part, moved
		// over to it; a frame that reads past it ends the walk)
		void Walk(Trace::Sample& s, CONTEXT& c, const uint8_t* copy, uintptr_t top, size_t size, uintptr_t base)
		{
			const auto relocate = [&](DWORD64& r) {
				if (r >= top && r < top + size)
					r = reinterpret_cast<uintptr_t>(copy) + (r - top);
			};
			const auto inCopy = [&](DWORD64 r) { return r >= reinterpret_cast<uintptr_t>(copy) && r + 8 <= reinterpret_cast<uintptr_t>(copy) + size; };
			c.Rsp = reinterpret_cast<uintptr_t>(copy);
			relocate(c.Rbp);
			size_t n = 0;
			__try
			{
				for (int i = 0; i < 64 && c.Rip && n < s.through.size() && inCopy(c.Rsp); ++i)
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
					relocate(c.Rbp);
				}
			}
			__except (EXCEPTION_EXECUTE_HANDLER)
			{
			}
		}

		// one of the threads stopped for a moment: where it is
		void SampleOne(Trace* t, size_t k, uintptr_t base)
		{
			static thread_local std::vector<uint8_t> copy(kStackCopy);
			if (SuspendThread(t->threads[k]) == static_cast<DWORD>(-1))
				return;
			CONTEXT c{};
			c.ContextFlags = CONTEXT_FULL;
			const bool got = GetThreadContext(t->threads[k], &c) != 0;
			const uintptr_t top = got ? c.Rsp : 0;
			const size_t size = got ? CopyStack(copy.data(), top, k < t->stackEnds.size() ? t->stackEnds[k] : 0) : 0;
			ResumeThread(t->threads[k]);
			if (!got)
				return;
			Trace::Sample& s = t->samples[t->taken % Trace::kMax];
			s = {};
			s.at = std::chrono::steady_clock::now();
			s.thread = static_cast<uint8_t>(k);
			Walk(s, c, copy.data(), top, size, base);
			++t->taken;
		}

		void TraceThread(Trace* t)
		{
			const auto base = reinterpret_cast<uintptr_t>(GetModuleHandleW(nullptr));
			while (t->running)
			{
				for (size_t k = 0; k < t->threads.size(); ++k)
					SampleOne(t, k, base);
				const auto until = std::chrono::steady_clock::now() + std::chrono::microseconds(1000);
				while (std::chrono::steady_clock::now() < until)
					std::this_thread::yield();
			}
		}
	}

	void Start(const std::vector<unsigned long>& others)
	{
		Stop();
		g_trace = std::make_unique<Trace>();
		g_trace->ids.push_back(GetCurrentThreadId());
		for (const unsigned long id : others)
			if (id != GetCurrentThreadId())
				g_trace->ids.push_back(id);
		for (const DWORD id : g_trace->ids)
			if (HANDLE h = OpenThread(THREAD_SUSPEND_RESUME | THREAD_GET_CONTEXT | THREAD_QUERY_INFORMATION, FALSE, id))
				g_trace->threads.push_back(h), g_trace->stackEnds.push_back(StackEnd(h));
		g_trace->running = true;
		g_trace->sampler = std::thread(TraceThread, g_trace.get());
	}

	void Stop()
	{
		if (!g_trace || !g_trace->running)
			return;
		g_trace->running = false;
		g_trace->sampler.join();
		for (HANDLE h : g_trace->threads)
			CloseHandle(h);
	}

	namespace
	{
		// one thread's samples between two moments, summed up
		std::string Summary(size_t thread, std::chrono::steady_clock::time_point from, std::chrono::steady_clock::time_point to)
		{
			const auto game = reinterpret_cast<uintptr_t>(GetModuleHandleW(nullptr));
			std::map<std::string, int> leaves, chains;
			int n = 0;
			const size_t taken = g_trace->taken, first = taken > Trace::kMax ? taken - Trace::kMax : 0;
			for (size_t i = first; i < taken; ++i)
			{
				const Trace::Sample& s = g_trace->samples[i % Trace::kMax];
				if (s.thread != thread || s.at < from || s.at > to)
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

	std::string Between(std::chrono::steady_clock::time_point from, std::chrono::steady_clock::time_point to)
	{
		if (!g_trace)
			return "not sampled";
		std::string all = Summary(0, from, to);
		for (size_t thread = 1; thread < g_trace->ids.size(); ++thread)
			all += std::format(" || thread {}: {}", g_trace->ids[thread], Summary(thread, from, to));
		return all;
	}
}
