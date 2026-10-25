#include "osm.hpp"

#include <fstream>

#include <nlohmann/json.hpp>

namespace worldgen::osm
{
	bool Load(const std::filesystem::path& dir, Data& out, std::string& error)
	{
		std::error_code ec;
		{
			std::ifstream in(dir / "box.json");
			if (!in)
				return error = "no box.json in " + dir.string() + " (tools/osm_fetch.py writes it)", false;
			const auto box = nlohmann::json::parse(in, nullptr, false);
			if (box.is_discarded())
				return error = "box.json is not JSON", false;
			out.south = box.value("south", 0.0), out.west = box.value("west", 0.0), out.north = box.value("north", 0.0), out.east = box.value("east", 0.0);
		}
		size_t files = 0;
		for (const auto& entry : std::filesystem::directory_iterator(dir, ec))
		{
			if (entry.path().extension() != ".json" || entry.path().filename() == "box.json")
				continue;
			std::ifstream in(entry.path(), std::ios::binary);
			const auto doc = nlohmann::json::parse(in, nullptr, false);
			if (doc.is_discarded() || !doc.contains("elements"))
				return error = entry.path().string() + " is not an Overpass answer", false;
			++files;
			for (const auto& e : doc["elements"])
			{
				const std::string type = e.value("type", "");
				const int64_t id = e.value("id", int64_t{0});
				Tags tags;
				if (e.contains("tags"))
					for (const auto& [k, v] : e["tags"].items())
						if (v.is_string())
							tags[k] = v.get<std::string>();
				if (type == "node")
				{
					if (e.contains("lat"))
						out.nodes[id] = {e["lat"].get<double>(), e["lon"].get<double>()};
					if (!tags.empty())
						out.nodeTags[id] = std::move(tags);
				}
				else if (type == "way")
				{
					Way& w = out.ways[id];
					if (e.contains("nodes") && w.nodes.empty())
						for (const auto& n : e["nodes"])
							w.nodes.push_back(n.get<int64_t>());
					if (!tags.empty())
						w.tags = std::move(tags);
				}
				else if (type == "relation")
				{
					Relation& r = out.relations[id];
					if (e.contains("members") && r.members.empty())
						for (const auto& m : e["members"])
							r.members.push_back({m.value("type", "w")[0], m.value("ref", int64_t{0}), m.value("role", "")});
					if (!tags.empty())
						r.tags = std::move(tags);
				}
			}
		}
		if (!files)
			return error = "no data files in " + dir.string(), false;
		return true;
	}
}
