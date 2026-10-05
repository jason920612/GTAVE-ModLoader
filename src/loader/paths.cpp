#include "paths.hpp"

#include <Windows.h>

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
}
