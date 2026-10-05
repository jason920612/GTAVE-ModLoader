// version.dll: forwards every export to the system version.dll and, inside the game only,
// loads the actual loader from ModLoader\ModLoader.dll next to it.
// Kept tiny and free of hooks on purpose: it rarely changes, so antivirus reputation built
// for it carries over between loader releases.
#include <Windows.h>

#include <cwchar>

#include "proxy.hpp"

namespace
{
	constexpr wchar_t kGameExe[] = L"GTA5_Enhanced.exe";
	constexpr wchar_t kCoreDll[] = L"ModLoader\\ModLoader.dll";

	const wchar_t* FileName(const wchar_t* path)
	{
		const wchar_t* slash = wcsrchr(path, L'\\');
		return slash ? slash + 1 : path;
	}

	bool IsGameProcess()
	{
		wchar_t exe[MAX_PATH];
		return GetModuleFileNameW(nullptr, exe, MAX_PATH) && _wcsicmp(FileName(exe), kGameExe) == 0;
	}

	void LoadCore(HMODULE self)
	{
		wchar_t path[MAX_PATH];
		const DWORD len = GetModuleFileNameW(self, path, MAX_PATH);
		if (!len || len >= MAX_PATH)
			return;
		*const_cast<wchar_t*>(FileName(path)) = L'\0';
		if (wcscat_s(path, kCoreDll) != 0)
			return;
		// Its own dependencies resolve from the ModLoader folder first.
		LoadLibraryExW(path, nullptr, LOAD_WITH_ALTERED_SEARCH_PATH);
	}
}

BOOL APIENTRY DllMain(HMODULE module, DWORD reason, LPVOID)
{
	if (reason == DLL_PROCESS_ATTACH)
	{
		DisableThreadLibraryCalls(module);
		if (!stub::proxy::LoadRealVersionDll())
			return FALSE;
		// The same version.dll is also picked up by PlayGTAV.exe and the BattlEye launcher.
		if (IsGameProcess())
			LoadCore(module);
	}
	return TRUE;
}
