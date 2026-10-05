#include "config.hpp"

#include <fstream>

#include <nlohmann/json.hpp>

#include "paths.hpp"

namespace loader::config
{
	namespace
	{
		Config g_config;

		NLOHMANN_JSON_SERIALIZE_ENUM(log::Level, {
			{log::Level::Debug, "debug"},
			{log::Level::Info, "info"},
			{log::Level::Warn, "warn"},
			{log::Level::Error, "error"},
		})

		void FromJson(const nlohmann::json& j, Config& c)
		{
			c.logLevel = j.value("logLevel", c.logLevel);
			c.menuKey = j.value("menuKey", c.menuKey);
			c.disabledMods = j.value("disabledMods", c.disabledMods);
			c.disabledAssets = j.value("disabledAssets", c.disabledAssets);
		}

		nlohmann::json ToJson(const Config& c)
		{
			return {
				{"logLevel", c.logLevel},
				{"menuKey", c.menuKey},
				{"disabledMods", c.disabledMods},
				{"disabledAssets", c.disabledAssets},
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
