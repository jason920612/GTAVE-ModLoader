#include <Windows.h>

#include <cwctype>
#include <string>

#include "bootstrap.hpp"
#include "config.hpp"
#include "log.hpp"
#include "paths.hpp"
#include "proxy.hpp"

namespace
{
	constexpr wchar_t kGameExe[] = L"GTA5_Enhanced.exe";
	constexpr wchar_t kNoBattlEye[] = L"-nobattleye";

	std::wstring Lower(std::wstring s)
	{
		for (auto& c : s)
			c = static_cast<wchar_t>(std::towlower(c));
		return s;
	}

	bool IsGameProcess()
	{
		wchar_t exe[MAX_PATH];
		GetModuleFileNameW(nullptr, exe, MAX_PATH);
		return Lower(std::filesystem::path(exe).filename().wstring()) == Lower(kGameExe);
	}

	bool IsBattlEyeDisabled()
	{
		return Lower(GetCommandLineW()).find(kNoBattlEye) != std::wstring::npos;
	}

	uint32_t GameBuildStamp()
	{
		auto base = reinterpret_cast<const uint8_t*>(GetModuleHandleW(nullptr));
		auto nt = reinterpret_cast<const IMAGE_NT_HEADERS*>(base + reinterpret_cast<const IMAGE_DOS_HEADER*>(base)->e_lfanew);
		return nt->FileHeader.TimeDateStamp;
	}

	void Start()
	{
		using namespace loader;

		paths::EnsureLayout();
		log::Init(paths::Get().log);
		log::Info("GTAV Enhanced ModLoader starting (build stamp {:#010x})", GameBuildStamp());

		if (!IsBattlEyeDisabled())
		{
			// Story-mode only: with BattlEye active the loader stays a plain version.dll proxy.
			log::Warn("game was not started with -nobattleye; loader disabled");
			return;
		}

		const auto& cfg = config::Load();
		log::SetLevel(cfg.logLevel);
		log::Info("mods dir: {}", paths::Get().mods.string());
		log::Info("assets dir: {}", paths::Get().assets.string());

		bootstrap::Install();
	}
}

BOOL APIENTRY DllMain(HMODULE module, DWORD reason, LPVOID)
{
	if (reason == DLL_PROCESS_ATTACH)
	{
		DisableThreadLibraryCalls(module);
		if (!loader::proxy::LoadRealVersionDll())
			return FALSE;
		// The same version.dll is also picked up by PlayGTAV.exe and the BattlEye launcher.
		if (IsGameProcess())
			Start();
	}
	return TRUE;
}
