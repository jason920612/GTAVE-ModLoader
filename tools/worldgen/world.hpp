#pragma once
#include <array>
#include <cmath>
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
		int centreLanes = 0;           // a two-way left turn lane down the middle (OSM lanes:both_ways): not one of those
		float laneWidth = 3.5f;
		bool sidewalks = false;
		bool sidewalkTagged = false; // (OSM said so - sidewalk=both/left/right/no/separate; else the build decides by the land)
		float sidewalkWidth = 0;     // (m; 0: by the road's kind - the build's, not saved)
		float parking = 0;           // (m: a parking lane's width each side, 0 none - the build's, not saved)
		bool bikeLanes = false;      // (OSM cycleway=lane: a painted bike lane by each kerb)
		bool curb = false;           // (a kerb and gutter without a sidewalk: a town's street - the build's, not saved)
		bool centreLine = false;     // (a dashed yellow line between the directions of a street its kind gives none: Miami-Dade's
		                             // residential streets - the build's, not saved)
		bool sidewalkLeft = true, sidewalkRight = true; // (with sidewalks: which sides have one, left and right of the points' way;
		                                                // the other a kerb and gutter - the build's, not saved)
		bool bridge = false, tunnel = false;
		int layer = 0;
		float speed = 0; // m/s (0: by kind)
		std::string name;
		std::string surface, smoothness; // OSM's (empty: not tagged)
	};

	struct Area
	{
		std::string kind;                   // park, grass, cemetery, beach, wood, water, parking, residential, commercial, ...
		std::vector<std::vector<P2>> outer; // closed rings (last point != first)
		std::vector<std::vector<P2>> inner;
		std::string name;                   // (the map's, when it has one)
	};

	struct Building
	{
		std::vector<P2> footprint; // a closed ring, anticlockwise
		float height = 0;
		std::string kind; // OSM building value
		std::string name;
		bool planned = false; // the world editor's: not there yet (its height as given, not measured)
		bool traced = false;  // Microsoft's footprint (world.json "source": "ms"), not the map's
	};

	struct Point
	{
		std::string kind; // tree, traffic_signals, stop, crossing (crossing:marked, crossing:unmarked when tagged so), fire_hydrant,
		                  // street_lamp, ...;
		                  // place:city, place:town, place:village, place:suburb, ... (with a name)
		P2 position{};
		std::string name;
	};

	struct Waterway
	{
		std::vector<P2> points;
		float width = 10; // m
		std::string kind; // canal, river, stream, ...
	};

	// A railway line (one track): OSM's railway kind - rail, light_rail, subway, monorail, narrow_gauge -, on a bridge
	// (elevated: a viaduct, a guideway) at a layer.
	struct Rail
	{
		std::vector<P2> points;
		std::string kind;
		bool bridge = false;
		int layer = 0;
	};

	// A barrier along a line (OSM barrier=*): fence (its material: chain_link, wood, metal, ...), wall, retaining_wall,
	// hedge, guard_rail; its height when tagged (0: not).
	struct Barrier
	{
		std::vector<P2> points;
		std::string kind, material;
		float height = 0;
	};

	// A way people walk (OSM highway=footway, path, cycleway, pedestrian, steps, bridleway; and a house's driveway, highway=
	// service + service=driveway: kind "driveway"): its kind - "sidewalk" (footway=sidewalk: a street's, drawn on its own),
	// "crossing" (footway=crossing), else the highway value -, its surface and width
	// when tagged (0: not), on a bridge at a layer.
	struct Walk
	{
		int64_t id = 0;
		std::vector<P2> points;
		std::string kind, surface;
		float width = 0;
		bool bridge = false, tunnel = false;
		int layer = 0;
	};

	// Latitude and longitude to game metres: (lat0, lon0) at (x0, y0), metres per degree at the box's middle.
	struct Projection
	{
		double lat0 = 0, lon0 = 0, x0 = 0, y0 = 0, kx = 0, ky = 0;
		static Projection Around(double south, double west, double north, double east, double x0, double y0);
		// (the place in the projection's plane on a lattice of 1/1024 m, then the land's origin - whole metres -: every
		// coordinate under 16384 m is then a float exactly, the same place the same number less the origin in every land - a
		// float of its own in each had two neighbours' bands half a millimetre apart, and the choices keyed to the place,
		// worldgen seeds.hpp, now and then not the same)
		P2 To(double lat, double lon) const
		{
			const auto snap = [](double v) { return std::round(v * 1024) / 1024; };
			return {static_cast<float>(x0 + snap((lon - lon0) * kx)), static_cast<float>(y0 + snap((lat - lat0) * ky))};
		}
	};

	// A brush stroke of the world editor's: painting the ground's class (park, beach, paved, water, urban, land) or shaping
	// the terrain (raise, lower, flatten), `radius` metres round its points.
	struct Stroke
	{
		std::string what; // the class painted, or the terrain's operation
		float radius = 10, amount = 1; // (amount: metres raised or lowered at the middle)
		std::vector<P2> points;
	};

	struct World
	{
		std::string name;
		Projection projection; // (kept for the data read later: elevation, aerial pictures)
		P2 min{}, max{}; // the land's box
		std::vector<std::vector<P2>> coastlines;
		std::vector<Area> areas;
		std::vector<Waterway> waterways;
		std::vector<Rail> rails;
		std::vector<Barrier> barriers;
		std::vector<Road> roads;
		std::vector<Walk> walks;
		std::vector<Building> buildings;
		std::vector<Point> points;
		std::string attribution; // the data's source
		// the world editor's (edits.json): ground painted, terrain shaped, areas cleared of buildings, the climate
		std::vector<Stroke> paint, terrain;
		std::vector<std::vector<P2>> demolish;
		std::string climate;
	};


	bool FromOsm(const osm::Data& data, const Projection& p, World& out, std::string& error);
	bool Save(const World& w, const std::filesystem::path& file, std::string& error);
	bool Load(const std::filesystem::path& file, World& w, std::string& error);

	// The world editor's changes (the worldedit mod's edits.json: {"roads": [road objects as world.json has them],
	// "removed": [road ids], "buildings": [{"footprint", "height", "kind"}], "demolish": [rings], "paint": [strokes],
	// "terrain": [strokes], "climate": name}): a road whose id the world has replaces it, the others are added; removed
	// ones go; new buildings are added, any whose middle lies in a demolished ring goes (worldgen build also leaves out
	// the scan's own there); the strokes and the climate are kept for the build.
	bool ApplyEdits(World& w, const std::filesystem::path& file, std::string& error);
	// The property rolls' word on the buildings (tools/worldgen/parcels.py's parcels.json: per building of the world, its
	// parcel's land use code, year built, living area, storeys): a footprint on a vacant parcel (codes 000, 010, 040, 070,
	// 080 with no living area: the house gone since the footprints were traced) is not built (as the editor's demolished);
	// a building the map calls only "yes" takes the kind its use says (house, static_caravan - a mobile home -, apartments,
	// retail, commercial, industrial, church, school, hospital, public); a height the map only guessed (its defaults: 4.5,
	// 6, 7, 9, 14, 18 m) the storeys' (a house's: 4.8 m for one storey - a mobile home 3.6 -, else 3 m a storey and the
	// roof; others 3.6 m a storey). For a build: not saved. False only when the file cannot be read.
	struct ParcelStats
	{
		size_t vacant = 0, retyped = 0, raised = 0;
	};
	bool ApplyParcels(World& w, const std::filesystem::path& file, ParcelStats& stats, std::string& error);

	// The buildings the map calls only "yes" (Microsoft's footprints all are) whose middle lies in the map's commercial or
	// retail land become "commercial", in its industrial land "industrial": shops, offices and workshops, not houses with
	// hip roofs and siding (the street pictures' block garage on a commercial street had become a sided house). For a
	// build: not saved. Returns how many.
	size_t KindsByLandUse(World& w);

	// A footprint whose middle lies in one of the editor's demolished areas.
	bool Demolished(const World& w, const std::vector<P2>& footprint);

	// The roads for the editor, compact (roads.bin, tools/worldgen and examples/worldedit read the same layout): 'WBRD',
	// u32 version 1, u32 count; each road: i64 id, u8 + kind, u16 + name, u8 forward, u8 backward, u8 flags (1 sidewalks,
	// 2 bridge, 4 tunnel), i8 layer, f32 lane width, f32 speed, u32 points, f32 x y each.
	bool SaveRoadIndex(const World& w, const std::filesystem::path& file, std::string& error);
}
