#include "paths.hpp"

#include <Windows.h>

#include <cwctype>
#include <map>
#include <mutex>
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

	std::filesystem::path CacheRoot()
	{
		const std::string& folder = config::Get().landCache;
		if (folder.empty())
			return {};
		std::wstring wide(static_cast<size_t>(MultiByteToWideChar(CP_UTF8, 0, folder.data(), static_cast<int>(folder.size()), nullptr, 0)), L'\0');
		MultiByteToWideChar(CP_UTF8, 0, folder.data(), static_cast<int>(folder.size()), wide.data(), static_cast<int>(wide.size()));
		return wide;
	}

	std::filesystem::path Cached(const std::filesystem::path& folder)
	{
		const std::filesystem::path root = CacheRoot();
		std::error_code ec;
		if (root.empty() || folder.parent_path() == root)
			return folder;
		// (decided once a run, at the start: a copy made while the game runs is not the pack it mounted - asked for again,
		// the land's reads had gone on from the hard disk with the memory's copy let go)
		static std::map<std::wstring, std::filesystem::path> decided;
		static std::mutex decidedMutex;
		{
			std::lock_guard lock(decidedMutex);
			if (const auto it = decided.find(folder.wstring()); it != decided.end())
				return it->second;
		}
		const auto decide = [&](const std::filesystem::path& to) {
			std::lock_guard lock(decidedMutex);
			return decided.try_emplace(folder.wstring(), to).first->second;
		};
		const std::filesystem::path copy = root / folder.filename();
		if (!std::filesystem::is_directory(copy, ec) || std::filesystem::exists(folder / L"dlc.rpf.new", ec))
			return decide(folder);
		bool any = false;
		for (const auto& entry : std::filesystem::directory_iterator(folder, ec))
		{
			std::error_code e1, e2, e3, e4;
			if (!entry.is_regular_file(e1))
				continue;
			const std::filesystem::path other = copy / entry.path().filename();
			if (std::filesystem::file_size(other, e2) != entry.file_size(e3) || e2 || e3 || std::filesystem::last_write_time(other, e4) != entry.last_write_time(e1) ||
			    e4 || e1)
				return decide(folder);
			any = true;
		}
		if (!any || ec)
			return decide(folder);
		// (the copy's last use, for the cache's order: its "used" file's time)
		if (HANDLE h = CreateFileW((copy / L"used").c_str(), GENERIC_WRITE, FILE_SHARE_READ, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
		    h != INVALID_HANDLE_VALUE)
			CloseHandle(h);
		return decide(copy);
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
					out.push_back(root == Get().mods ? entry.path() : Cached(entry.path()));
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
				return Cached(root / name);
		return Get().mods / name;
	}
}
