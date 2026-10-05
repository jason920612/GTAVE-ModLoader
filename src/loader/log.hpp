#pragma once
#include <filesystem>
#include <format>
#include <string_view>

namespace loader::log
{
	enum class Level { Debug, Info, Warn, Error };

	void Init(const std::filesystem::path& file);
	void SetLevel(Level level);
	void Write(Level level, std::string_view message);

	template<class... Args>
	void Debug(std::format_string<Args...> fmt, Args&&... args) { Write(Level::Debug, std::format(fmt, std::forward<Args>(args)...)); }
	template<class... Args>
	void Info(std::format_string<Args...> fmt, Args&&... args) { Write(Level::Info, std::format(fmt, std::forward<Args>(args)...)); }
	template<class... Args>
	void Warn(std::format_string<Args...> fmt, Args&&... args) { Write(Level::Warn, std::format(fmt, std::forward<Args>(args)...)); }
	template<class... Args>
	void Error(std::format_string<Args...> fmt, Args&&... args) { Write(Level::Error, std::format(fmt, std::forward<Args>(args)...)); }
}
