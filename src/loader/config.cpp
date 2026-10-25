#include "config.hpp"

#include <fstream>

#include <nlohmann/json.hpp>

#include "paths.hpp"

namespace loader::log
{
	// Must live in the enum's namespace so nlohmann finds it by ADL.
	NLOHMANN_JSON_SERIALIZE_ENUM(Level, {
		{Level::Debug, "debug"},
		{Level::Info, "info"},
		{Level::Warn, "warn"},
		{Level::Error, "error"},
	})
}

namespace loader::config
{
	namespace
	{
		Config g_config;

		void FromJson(const nlohmann::json& j, Config& c)
		{
			c.logLevel = j.value("logLevel", c.logLevel);
			c.menuKey = j.value("menuKey", c.menuKey);
			c.disabledMods = j.value("disabledMods", c.disabledMods);
			c.disabledAssets = j.value("disabledAssets", c.disabledAssets);
			c.crossmapUrl = j.value("crossmapUrl", c.crossmapUrl);
			c.crossmapAutoUpdate = j.value("crossmapAutoUpdate", c.crossmapAutoUpdate);
			c.replaceLandingPage = j.value("replaceLandingPage", c.replaceLandingPage);
			c.debugDisableScriptHook = j.value("debugDisableScriptHook", c.debugDisableScriptHook);
			c.debugWatchLanding = j.value("debugWatchLanding", c.debugWatchLanding);
			c.debugWatchFile = j.value("debugWatchFile", c.debugWatchFile);
			c.debugRemote = j.value("debugRemote", c.debugRemote);
			c.debugWatchBoot = j.value("debugWatchBoot", c.debugWatchBoot);
			c.debugPoseWatch = j.value("debugPoseWatch", c.debugPoseWatch);
			c.debugDlssTrace = j.value("debugDlssTrace", c.debugDlssTrace);
			c.pauseMenuModsTab = j.value("pauseMenuModsTab", c.pauseMenuModsTab);
			c.replaceBrowser = j.value("replaceBrowser", c.replaceBrowser);
			c.aiEnabled = j.value("aiEnabled", c.aiEnabled);
			c.dlssUpgrade = j.value("dlssUpgrade", c.dlssUpgrade);
			c.dlssPreset = j.value("dlssPreset", c.dlssPreset);
			c.fourthCharacter = j.value("fourthCharacter", c.fourthCharacter);
			c.ue5Renderer = j.value("ue5Renderer", c.ue5Renderer);
			c.ue5Editor = j.value("ue5Editor", c.ue5Editor);
			c.ue5Project = j.value("ue5Project", c.ue5Project);
			c.ue5Pack = j.value("ue5Pack", c.ue5Pack);
			c.ue5Key = j.value("ue5Key", c.ue5Key);
			if (c.ue5Key == "F7")
				c.ue5Key = "F11"; // (the old default, only ever saved as such: F7 is the trainer's)
			c.ue5Preset = j.value("ue5Preset", c.ue5Preset);
			c.ue5ExposureMatch = j.value("ue5ExposureMatch", c.ue5ExposureMatch);
			c.ue5GradeMatch = j.value("ue5GradeMatch", c.ue5GradeMatch);
			c.ue5Particles = j.value("ue5Particles", c.ue5Particles);
			c.ue5GameResolution = j.value("ue5GameResolution", c.ue5GameResolution);
			c.ue5GameDetail = j.value("ue5GameDetail", c.ue5GameDetail);
			c.ue5GameMemory = j.value("ue5GameMemory", c.ue5GameMemory);
		}

		nlohmann::json ToJson(const Config& c)
		{
			return {
				{"logLevel", c.logLevel},
				{"menuKey", c.menuKey},
				{"disabledMods", c.disabledMods},
				{"disabledAssets", c.disabledAssets},
				{"crossmapUrl", c.crossmapUrl},
				{"crossmapAutoUpdate", c.crossmapAutoUpdate},
				{"replaceLandingPage", c.replaceLandingPage},
				{"debugDisableScriptHook", c.debugDisableScriptHook},
				{"debugWatchLanding", c.debugWatchLanding},
				{"debugWatchFile", c.debugWatchFile},
				{"debugRemote", c.debugRemote},
				{"debugWatchBoot", c.debugWatchBoot},
				{"debugPoseWatch", c.debugPoseWatch},
				{"debugDlssTrace", c.debugDlssTrace},
				{"pauseMenuModsTab", c.pauseMenuModsTab},
				{"replaceBrowser", c.replaceBrowser},
				{"aiEnabled", c.aiEnabled},
				{"dlssUpgrade", c.dlssUpgrade},
				{"dlssPreset", c.dlssPreset},
				{"fourthCharacter", c.fourthCharacter},
				{"ue5Renderer", c.ue5Renderer},
				{"ue5Editor", c.ue5Editor},
				{"ue5Project", c.ue5Project},
				{"ue5Pack", c.ue5Pack},
				{"ue5Key", c.ue5Key},
				{"ue5Preset", c.ue5Preset},
				{"ue5ExposureMatch", c.ue5ExposureMatch},
				{"ue5GradeMatch", c.ue5GradeMatch},
				{"ue5Particles", c.ue5Particles},
				{"ue5GameResolution", c.ue5GameResolution},
				{"ue5GameDetail", c.ue5GameDetail},
				{"ue5GameMemory", c.ue5GameMemory},
			};
		}
	}

	Config& Load()
	{
		std::ifstream in(paths::Get().config);
		if (!in)
		{
			log::Info("loader.json not found, writing defaults");
			Save();
			return g_config;
		}

		try
		{
			FromJson(nlohmann::json::parse(in), g_config);
			in.close();
			Save(); // adds settings introduced by newer loader versions
		}
		catch (const std::exception& e)
		{
			// Keep the broken file for the user to fix; run with defaults.
			log::Error("loader.json is invalid, using defaults: {}", e.what());
			g_config = {};
		}
		return g_config;
	}

	Config& Get()
	{
		return g_config;
	}

	bool Save()
	{
		std::ofstream out(paths::Get().config, std::ios::trunc);
		if (!out)
		{
			log::Error("could not write loader.json");
			return false;
		}
		out << ToJson(g_config).dump(2) << '\n';
		return static_cast<bool>(out);
	}
}
