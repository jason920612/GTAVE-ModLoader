#pragma once
#include <array>
#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

#include "osm.hpp"

// The world description (research/phase0.md §61.15): new land in game coordinates, as the generator builds it and the
// in-game editor changes it. From OpenStreetMap: the coastline (land on its left), areas (parks, beaches, water, land
// use), roads (kind, lanes each way, width, bridges), buildings (footprint, height), points (trees, traffic signals).
namespace worldgen::world
{
	using P2 = std::array<float, 2>;

	struct Road
	{
		int64_t id = 0;
		std::vector<P2> points;
		std::string kind; // OSM highway: motorway, primary, residential, service, ..._link
		int forward = 1, backward = 1; // lanes along the points / against them
		float laneWidth = 3.5f;
		bool sidewalks = false;
		bool bridge = false, tunnel = false;
		int layer = 0;
		float speed = 0; // m/s (0: by kind)
		std::string name;
		std::string surface, smoothness; // OSM's (empty: not tagged)
	};

	struct Area
	{
		std::string kind;                   // park, grass, beach, wood, water, parking, residential, commercial, ...
		std::vector<std::vector<P2>> outer; // closed rings (last point != first)
		std::vector<std::vector<P2>> inner;
	};

	struct Building
	{
		std::vector<P2> footprint; // a closed ring, anticlockwise
		float height = 0;
		std::string kind; // OSM building value
		std::string name;
	};

	struct Point
	{
		std::string kind; // tree, traffic_signals, ...
		P2 position{};
	};

	struct Waterway
	{
		std::vector<P2> points;
		float width = 10; // m
		std::string kind; // canal, river, stream, ...
	};

	// Latitude and longitude to game metres: (lat0, lon0) at (x0, y0), metres per degree at the box's middle.
	struct Projection
	{
		double lat0 = 0, lon0 = 0, x0 = 0, y0 = 0, kx = 0, ky = 0;
		static Projection Around(double south, double west, double north, double east, double x0, double y0);
		P2 To(double lat, double lon) const { return {static_cast<float>(x0 + (lon - lon0) * kx), static_cast<float>(y0 + (lat - lat0) * ky)}; }
	};

	struct World
	{
		std::string name;
		Projection projection; // (kept for the data read later: elevation, aerial pictures)
		P2 min{}, max{}; // the land's box
		std::vector<std::vector<P2>> coastlines;
		std::vector<Area> areas;
		std::vector<Waterway> waterways;
		std::vector<Road> roads;
		std::vector<Building> buildings;
		std::vector<Point> points;
		std::string attribution; // the data's source
	};


	bool FromOsm(const osm::Data& data, const Projection& p, World& out, std::string& error);
	bool Save(const World& w, const std::filesystem::path& file, std::string& error);
	bool Load(const std::filesystem::path& file, World& w, std::string& error);

	// The world editor's changes (the worldedit mod's edits.json: {"roads": [road objects as world.json has them],
	// "removed": [road ids]}): a road whose id the world has replaces it, the others are added; removed ones go.
	bool ApplyEdits(World& w, const std::filesystem::path& file, std::string& error);

	// The roads for the editor, compact (roads.bin, tools/worldgen and examples/worldedit read the same layout): 'WBRD',
	// u32 version 1, u32 count; each road: i64 id, u8 + kind, u16 + name, u8 forward, u8 backward, u8 flags (1 sidewalks,
	// 2 bridge, 4 tunnel), i8 layer, f32 lane width, f32 speed, u32 points, f32 x y each.
	bool SaveRoadIndex(const World& w, const std::filesystem::path& file, std::string& error);
}
