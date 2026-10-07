#include "story.hpp"

#include <Windows.h>
#include <ShlObj.h>

#include <algorithm>
#include <chrono>
#include <filesystem>

#include "../convert/resource.hpp"
#include "natives.hpp"
#include "scripts.hpp"

namespace loader::game::story
{
	namespace
	{
		constexpr uint64_t kPlayerPedId = 0xD80958FC74E988A6;
		constexpr uint64_t kGetEntityModel = 0x9F47B058362C84B5;
		constexpr uint64_t kIsLoadingScreenActive = 0x10D0A8F259E93EC9;
		constexpr uint32_t kAutosaveRequest = 102550; // +8 "saving in progress", +10 pending requests

		int32_t g_character = -1;
		bool g_loading = false;
		std::filesystem::file_time_type g_lastSave{};
		bool g_saveKnown = false;
		std::chrono::steady_clock::time_point g_nextSaveCheck{};

		// Newest change time of the story save files (Documents\Rockstar Games\GTAV Enhanced\Profiles\*\SGTA5*).
		std::filesystem::file_time_type LastGameSave()
		{
			static const std::filesystem::path profiles = [] {
				PWSTR documents = nullptr;
				std::filesystem::path path;
				if (SUCCEEDED(SHGetKnownFolderPath(FOLDERID_Documents, 0, nullptr, &documents)))
					path = std::filesystem::path(documents) / L"Rockstar Games" / L"GTAV Enhanced" / L"Profiles";
				CoTaskMemFree(documents);
				return path;
			}();
			std::filesystem::file_time_type newest{};
			std::error_code ec;
			for (const auto& profile : std::filesystem::directory_iterator(profiles, ec))
				for (const auto& file : std::filesystem::directory_iterator(profile.path(), ec))
					if (file.path().filename().wstring().starts_with(L"SGTA5"))
						newest = std::max(newest, file.last_write_time(ec));
			return newest;
		}

		int32_t ReadCharacter()
		{
			static const uint32_t models[3] = {convert::Joaat("player_zero"), convert::Joaat("player_one"), convert::Joaat("player_two")};
			const auto ped = natives::Invoke<int32_t>(kPlayerPedId);
			const auto model = natives::Invoke<uint32_t>(kGetEntityModel, ped);
			for (int32_t i = 0; i < 3; ++i)
				if (model == models[i])
					return i;
			return -1;
		}
	}

	Changes Update()
	{
		Changes changes;
		if (const int32_t c = ReadCharacter(); c != g_character)
		{
			g_character = c;
			changes.character = true;
		}
		if (const bool loading = natives::Invoke<int32_t>(kIsLoadingScreenActive) != 0; loading != g_loading)
		{
			g_loading = loading;
			changes.loading = loading;
		}
		// The save folder is looked at once a second.
		if (const auto now = std::chrono::steady_clock::now(); now >= g_nextSaveCheck)
		{
			g_nextSaveCheck = now + std::chrono::seconds(1);
			const auto save = LastGameSave();
			changes.saved = g_saveKnown && save != g_lastSave;
			g_lastSave = save;
			g_saveKnown = true;
		}
		return changes;
	}

	int32_t Character()
	{
		return g_character;
	}

	bool RequestAutosave()
	{
		int64_t* request = scripts::Global(kAutosaveRequest);
		if (!request || ((request[8] & 0xFFFFFFFF) ? request[10] > 0 : request[10] > 1))
			return false;
		++request[10];
		return true;
	}
}
