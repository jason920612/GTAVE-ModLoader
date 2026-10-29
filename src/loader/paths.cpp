#include "paths.hpp"

#include <Windows.h>

#include <cwctype>
#include <set>
#include <string>

#include "config.hpp"

namespace loader::paths
{
	const Paths& Get()
	{
		static const Paths paths = [] {
			wchar_t exe[MAX_PATH];
			GetModuleFileNameW(nullptr, exe, MAX_PATH);

			Paths p;
			p.gameDir = std::filesystem::path(exe).parent_path();
			p.root = p.gameDir / L"ModLoader";
			p.mods = p.root / L"mods";
			p.assets = p.root / L"assets";
			p.config = p.root / L"loader.json";
			p.log = p.root / L"loader.log";
			return p;
		}();
		return paths;
	}

	bool EnsureLayout()
	{
		std::error_code ec;
		bool ok = true;
		for (const auto* dir : {&Get().root, &Get().mods, &Get().assets})
		{
			std::filesystem::create_directories(*dir, ec);
			ok &= !ec;
		}
		return ok;
	}

	std::vector<std::filesystem::path> LandRoots()
	{
		std::vector<std::filesystem::path> out;
		for (const std::string& folder : config::Get().landFolders)
		{
			std::wstring wide(static_cast<size_t>(MultiByteToWideChar(CP_UTF8, 0, folder.data(), static_cast<int>(folder.size()), nullptr, 0)), L'\0');
			MultiByteToWideChar(CP_UTF8, 0, folder.data(), static_cast<int>(folder.size()), wide.data(), static_cast<int>(wide.size()));
			std::error_code ec;
			if (!wide.empty() && std::filesystem::is_directory(wide, ec))
				out.emplace_back(wide);
		}
		return out;
	}

	std::vector<std::filesystem::path> LandFolders()
	{
		std::vector<std::filesystem::path> out;
		std::set<std::wstring> names;
		std::vector<std::filesystem::path> roots{Get().mods};
		for (auto& root : LandRoots())
			roots.push_back(std::move(root));
		for (const auto& root : roots)
		{
			std::error_code ec;
			for (const auto& entry : std::filesystem::directory_iterator(root, ec))
			{
				std::wstring name = entry.path().filename().wstring();
				for (auto& c : name)
					c = static_cast<wchar_t>(std::towlower(c));
				if (entry.is_directory(ec) && std::filesystem::is_regular_file(entry.path() / L"land.json", ec) && names.insert(name).second)
					out.push_back(entry.path());
			}
		}
		return out;
	}

	std::filesystem::path LandDir(std::string_view land)
	{
		const std::filesystem::path name(std::u8string(land.begin(), land.end()));
		std::error_code ec;
		if (std::filesystem::is_directory(Get().mods / name, ec))
			return Get().mods / name;
		for (const auto& root : LandRoots())
			if (std::filesystem::is_directory(root / name, ec))
				return root / name;
		return Get().mods / name;
	}
}
