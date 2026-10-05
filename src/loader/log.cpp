#include "log.hpp"

#include <Windows.h>

#include <chrono>
#include <cstdio>
#include <deque>
#include <mutex>

namespace loader::log
{
	namespace
	{
		std::mutex g_mutex;
		FILE* g_file = nullptr;
		Level g_level = Level::Info;
		const auto g_start = std::chrono::steady_clock::now();
		constexpr size_t kRecentLines = 500;
		std::deque<Line> g_recent;

		constexpr const char* Name(Level level)
		{
			switch (level)
			{
			case Level::Debug: return "DEBUG";
			case Level::Info: return "INFO ";
			case Level::Warn: return "WARN ";
			default: return "ERROR";
			}
		}
	}

	void Init(const std::filesystem::path& file)
	{
		std::lock_guard lock(g_mutex);
		if (g_file)
			return;
		// Shared read so the log can be tailed while the game runs.
		g_file = _wfsopen(file.c_str(), L"w", _SH_DENYWR);
	}

	void SetLevel(Level level)
	{
		g_level = level;
	}

	void Write(Level level, std::string_view message)
	{
		if (level < g_level)
			return;

		const auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - g_start).count();
		const auto line = std::format("[{:>8}ms] [{}] [T{:>5}] {}\n", ms, Name(level), GetCurrentThreadId(), message);

		std::lock_guard lock(g_mutex);
		g_recent.push_back({level, line.substr(0, line.size() - 1)});
		if (g_recent.size() > kRecentLines)
			g_recent.pop_front();
		if (g_file)
		{
			fwrite(line.data(), 1, line.size(), g_file);
			fflush(g_file);
		}
		OutputDebugStringA(line.c_str());
	}

	std::vector<Line> Recent()
	{
		std::lock_guard lock(g_mutex);
		return {g_recent.begin(), g_recent.end()};
	}
}
