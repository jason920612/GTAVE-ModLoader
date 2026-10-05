// Loads the real system version.dll and fills the jump table used by proxy_version.asm.
#include "proxy.hpp"

#include <Windows.h>

#include <iterator>

#include "proxy_version_names.hpp"

extern "C" void* g_versionExports[std::size(kVersionExportNames)] = {};

namespace loader::proxy
{
	bool LoadRealVersionDll()
	{
		wchar_t path[MAX_PATH];
		const UINT len = GetSystemDirectoryW(path, MAX_PATH);
		if (len == 0 || len + 13 >= MAX_PATH)
			return false;
		wcscat_s(path, L"\\version.dll");

		HMODULE real = LoadLibraryW(path);
		if (!real)
			return false;

		for (size_t i = 0; i < std::size(kVersionExportNames); ++i)
		{
			g_versionExports[i] = reinterpret_cast<void*>(GetProcAddress(real, kVersionExportNames[i]));
			if (!g_versionExports[i])
				return false;
		}
		return true;
	}
}
