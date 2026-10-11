// mujoco.dll is delay-loaded from the mod's own folder (ModLoader\mods\basketball\): a DLL directly in mods\ would be
// taken for a mod. Loading it here by full path makes the delay-load stubs find it already in the process.
#include <Windows.h>

#include <string>

namespace bball
{
	bool LoadMujoco(const std::wstring& modDir)
	{
		return LoadLibraryExW((modDir + L"mujoco.dll").c_str(), nullptr, LOAD_WITH_ALTERED_SEARCH_PATH) != nullptr;
	}
}
