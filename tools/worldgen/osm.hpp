#pragma once
#include <cstdint>
#include <filesystem>
#include <map>
#include <string>
#include <unordered_map>
#include <vector>

// OpenStreetMap data as tools/osm_fetch.py saves it (Overpass JSON, several files of one box, overlapping): nodes,
// ways and relations, merged by id. The data is OpenStreetMap's (ODbL, (c) OpenStreetMap contributors).
namespace worldgen::osm
{
	using Tags = std::map<std::string, std::string>;

	struct Node
	{
		double lat = 0, lon = 0;
	};

	struct Way
	{
		std::vector<int64_t> nodes;
		Tags tags;
	};

	struct Member
	{
		char type = 'w'; // n, w, r
		int64_t ref = 0;
		std::string role;
	};

	struct Relation
	{
		std::vector<Member> members;
		Tags tags;
	};

	struct Data
	{
		double south = 0, west = 0, north = 0, east = 0; // the box (box.json)
		std::unordered_map<int64_t, Node> nodes;
		std::unordered_map<int64_t, Tags> nodeTags; // nodes with tags (trees, signals...)
		std::unordered_map<int64_t, Way> ways;
		std::unordered_map<int64_t, Relation> relations;
	};

	bool Load(const std::filesystem::path& dir, Data& out, std::string& error);

	// A tag's value, or "".
	inline const std::string& Tag(const Tags& t, const std::string& key)
	{
		static const std::string none;
		const auto it = t.find(key);
		return it == t.end() ? none : it->second;
	}
}
