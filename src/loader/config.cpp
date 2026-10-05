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
			c.debugWatchBoot = j.value("debugWatchBoot", c.debugWatchBoot);
			c.experimentalPauseMenu = j.value("experimentalPauseMenu", c.experimentalPauseMenu);
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
				{"debugWatchBoot", c.debugWatchBoot},
				{"experimentalPauseMenu", c.experimentalPauseMenu},
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
