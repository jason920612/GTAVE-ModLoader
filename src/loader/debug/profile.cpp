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
}
