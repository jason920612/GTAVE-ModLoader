// worldgen: the world builder's compiler (research/phase0.md §61). From the player's own game files.
//
//   worldgen roundtrip --game <dir> --keys <ng.bin>
//       every road-network region the game uses read, written back with our writer and read again: must be the same
//   worldgen ynv-roundtrip --game <dir> --keys <ng.bin>
//       every navmesh the game uses read, written back with our writer and read again: must be the same
//   worldgen ynv-reindex --game <dir> --keys <ng.bin> --out <dir> [--width n]
//       every navmesh renumbered for a wider grid (default 147 squares; the game has 100): own index, adjacency, polygons,
//       special links
//   worldgen extract --game <dir> --keys <ng.bin> --out <dir> --name <pattern>
//       game files by name ("*" matches any run of characters), as stored; a resource also as its two blocks
//   worldgen extract-ynd --game <dir> --keys <ng.bin> --out <dir>
//       the road-network region files the game uses, as they are
//   worldgen testroad --game <dir> --keys <ng.bin> --out <dir> --path x,y,z/x,y,z[/...] [--spacing m] [--lanes n]
//       [--template x,y]
//       a two-way test road along the polyline for the 1024 m grid (a road node of the game copied: its flags, switched
//       on; n lanes each way), positions relative to the region's corner outside +-8192 m
//   worldgen navmesh --game <dir> --keys <ng.bin> --ymap <file> --out <dir> [--surface pavement|ground|road] [--obj <file>]
//       pedestrians' navmeshes (navgen.hpp) for the entities of a map file: each one's collision (its drawable's) placed
//       as the entity is; every walkable face of that kind (pavement by default). --obj: the collision as an OBJ file
//   worldgen pack --game <dir> --keys <ng.bin> --in <dir> --out <dlc.rpf> --name <pack>
//       a DLC pack of the files in the folder (ModLoader\mods\<name>\dlc.rpf): a plain streaming archive, so new files
//       (maps, navmeshes...) are registered as new
//   worldgen regrid --game <dir> --keys <ng.bin> --out <dir>
//       the road network in 1024 m regions from -16384 (still 32 x 32; the loader's worldPathGrid): every node goes to
//       the region its position falls in, regions sorted (vehicle nodes, then pedestrian nodes, each by y) and renumbered,
//       links and junction refs pointed at the new numbers, junctions (and their height maps) moved with the nodes that
//       use them. Cayo Perico's set (1024 + N) on its own. Writes nodes<N>.ynd for every region with nodes and an empty
//       one for every old name that has none now (so the game's file of that name is not read into the new region).
//       Positions stay as they are: these regions lie within +-8192 m, where a region's origin is 0.
#include <Windows.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <format>
#include <fstream>
#include <functional>
#include <map>
#include <memory>
#include <chrono>
#include <set>
#include <sstream>
#include <string>
#include <vector>

#include "../ue5export/drawable.hpp"
#include "convert/archive.hpp"
#include "convert/pso.hpp"
#include "build.hpp"
#include "catalog.hpp"
#include "osm.hpp"
#include "pbf.hpp"
#include "raster.hpp"
#include "world.hpp"
#include "convert/ytd.hpp"
#include "elevation.hpp"
#include "roadedit.hpp"
#include "facades.hpp"
#include "structures.hpp"
#include "flora.hpp"
#include "texture.hpp"
#include "mapfiles.hpp"
#include "terrain.hpp"
#include "ybn.hpp"
#include "ydr.hpp"
#include "../ue5export/gamefiles.hpp"
#include "../ue5export/meta.hpp"
#include "../ue5export/ng.hpp"
#include "../ue5export/pso.hpp"
#include "bounds.hpp"
#include "meta.hpp"
#include "navgen.hpp"
#include "pages.hpp"
#include "ynd.hpp"
#include "ynv.hpp"

namespace fs = std::filesystem;
using loader::convert::Bytes;

namespace
{
	std::map<std::string, std::string> g_options;

	int Fail(const std::string& message)
	{
		std::fprintf(stderr, "%s\n", message.c_str());
		return 1;
	}

	struct Loaded
	{
		uint32_t file; // the file's number (area id, + 1024 for Cayo Perico's set)
		worldgen::ynd::Region region;
	};

	bool LoadAll(const ue5export::GameFiles& files, std::vector<Loaded>& out, std::string& error)
	{
		for (uint32_t n = 0; n < 2048; ++n)
		{
			const auto* f = files.Find("nodes" + std::to_string(n) + ".ynd");
			if (!f)
				continue;
			Bytes data;
			Loaded l{n, {}};
			if (!files.Read(*f, data, error) || !worldgen::ynd::Read(data, l.region, error))
			{
				error = f->path + ": " + error;
				return false;
			}
			out.push_back(std::move(l));
		}
		return true;
	}

	bool Same(const worldgen::ynd::Region& a, const worldgen::ynd::Region& b, std::string& why)
	{
		auto bytes = [](const auto& v) { return std::string(reinterpret_cast<const char*>(v.data()), v.size() * sizeof(v[0])); };
		if (a.vehicleNodes != b.vehicleNodes || a.unknown48 != b.unknown48)
			why = "counts";
		else if (bytes(a.nodes) != bytes(b.nodes))
			why = "nodes";
		else if (bytes(a.links) != bytes(b.links))
			why = "links";
		else if (bytes(a.junctions) != bytes(b.junctions))
			why = "junctions";
		else if (a.heights != b.heights)
			why = "heights";
		else if (bytes(a.refs) != bytes(b.refs))
			why = "refs";
		return why.empty();
	}

	int Roundtrip(const ue5export::GameFiles& files)
	{
		std::vector<Loaded> all;
		std::string error;
		if (!LoadAll(files, all, error))
			return Fail(error);
		size_t nodes = 0, bad = 0;
		for (const Loaded& l : all)
		{
			Bytes written;
			worldgen::ynd::Region again;
			std::string why;
			if (!worldgen::ynd::Write(l.region, written, error) || !worldgen::ynd::Read(written, again, error))
				why = error;
			else
				Same(l.region, again, why);
			if (!why.empty())
			{
				++bad;
				std::printf("nodes%u: %s\n", l.file, why.c_str());
			}
			nodes += l.region.nodes.size();
		}
		std::printf("%zu regions, %zu nodes: %zu differ after writing them again\n", all.size(), nodes, bad);
		return bad ? 1 : 0;
	}

	// One set of regions (the base set, or Cayo Perico's) into 1024 m regions.
	bool Regrid(const std::vector<Loaded>& set, uint32_t fileBase, const fs::path& out, std::set<uint32_t>& written, std::string& error)
	{
		using namespace worldgen::ynd;
		struct Where
		{
			uint32_t area, index;
		};
		std::map<uint32_t, const Region*> byArea;
		for (const Loaded& l : set)
			byArea[l.file - fileBase] = &l.region;
		// every node's new region, and the order inside it
		struct Item
		{
			uint32_t oldArea, oldIndex;
			bool vehicle;
			int16_t x, y;
		};
		std::map<uint32_t, std::vector<Item>> regions;
		for (const auto& [area, r] : byArea)
			for (uint32_t i = 0; i < r->nodes.size(); ++i)
			{
				const Node& n = r->nodes[i];
				// 0.25 m units: 1024 m = 4096 units, the grid from -16384 m = -65536 units
				const int cx = (n.X() + 65536) / 4096, cy = (n.Y() + 65536) / 4096;
				regions[static_cast<uint32_t>(cy * 32 + cx)].push_back({area, i, i < r->vehicleNodes, n.X(), n.Y()});
			}
		std::map<uint64_t, Where> moved; // (old area << 32 | old index) -> new place
		for (auto& [area, items] : regions)
		{
			std::stable_sort(items.begin(), items.end(), [](const Item& a, const Item& b) {
				if (a.vehicle != b.vehicle)
					return a.vehicle;
				return a.y < b.y;
			});
			for (uint32_t i = 0; i < items.size(); ++i)
				moved[(static_cast<uint64_t>(items[i].oldArea) << 32) | items[i].oldIndex] = {area, i};
		}
		size_t lost = 0;
		for (auto& [area, items] : regions)
		{
			Region n;
			std::map<uint64_t, uint16_t> junctionAt; // (old area << 32 | old junction) -> new index
			for (uint32_t i = 0; i < items.size(); ++i)
			{
				const Item& it = items[i];
				const Region& old = *byArea[it.oldArea];
				Node node = old.nodes[it.oldIndex];
				node.SetArea(static_cast<uint16_t>(area));
				node.SetIndex(static_cast<uint16_t>(i));
				node.SetFirstLink(static_cast<uint16_t>(n.links.size()));
				for (uint32_t k = 0; k < node.LinkCount(); ++k)
				{
					Link link = old.links[old.nodes[it.oldIndex].FirstLink() + k];
					const auto to = moved.find((static_cast<uint64_t>(link.area) << 32) | link.node);
					if (to == moved.end())
						++lost; // a link into a region of the other set (or none): kept as it was
					else
						link.area = static_cast<uint16_t>(to->second.area), link.node = static_cast<uint16_t>(to->second.index);
					n.links.push_back(link);
				}
				if (it.vehicle)
					++n.vehicleNodes;
				n.nodes.push_back(node);
				// its junction refs (and the junctions they name)
				for (const JunctionRef& ref : old.refs)
				{
					if (ref.area != it.oldArea || ref.node != it.oldIndex)
						continue;
					const uint64_t key = (static_cast<uint64_t>(it.oldArea) << 32) | ref.junction;
					auto j = junctionAt.find(key);
					if (j == junctionAt.end())
					{
						Junction junction = old.junctions[ref.junction];
						const size_t cells = static_cast<size_t>(junction.width) * junction.height;
						const uint16_t at = static_cast<uint16_t>(n.heights.size());
						n.heights.insert(n.heights.end(), old.heights.begin() + junction.heightmap, old.heights.begin() + junction.heightmap + cells);
						junction.heightmap = at;
						j = junctionAt.emplace(key, static_cast<uint16_t>(n.junctions.size())).first;
						n.junctions.push_back(junction);
					}
					n.refs.push_back({static_cast<uint16_t>(area), static_cast<uint16_t>(i), j->second, ref.unused});
				}
			}
			if (n.heights.size() > 0xFFFF)
			{
				error = "region " + std::to_string(area) + ": junction height maps over 64 KB";
				return false;
			}
			std::sort(n.refs.begin(), n.refs.end(), [](const JunctionRef& a, const JunctionRef& b) { return a.node < b.node; });
			Bytes data;
			if (!Write(n, data, error))
				return false;
			const uint32_t file = fileBase + area;
			std::ofstream(out / ("nodes" + std::to_string(file) + ".ynd"), std::ios::binary).write(reinterpret_cast<const char*>(data.data()), data.size());
			written.insert(file);
			std::printf("nodes%u: %zu nodes (%u vehicle), %zu links, %zu junctions\n", file, n.nodes.size(), n.vehicleNodes, n.links.size(),
			    n.junctions.size());
		}
		if (lost)
			std::printf("(%zu links into regions outside this set kept as they were)\n", lost);
		return true;
	}

	// The origin of a 1024 m region (0.25 m units): 0 within +-8192 m, else its south-west corner (as the loader sets it).
	void Origin(uint32_t area, int& x, int& y)
	{
		const int cx = static_cast<int>(area % 32), cy = static_cast<int>(area / 32);
		const bool inside = cx >= 8 && cx < 24 && cy >= 8 && cy < 24;
		x = inside ? 0 : (cx * 1024 - 16384) * 4;
		y = inside ? 0 : (cy * 1024 - 16384) * 4;
	}

	// A two-way test road along a polyline (a node every --spacing m on each straight) for the 1024 m grid, made of a road
	// node of the game (its flags, switched on; --lanes lanes each way): nodes<N>.ynd for the regions it crosses.
	int TestRoad(const ue5export::GameFiles& files)
	{
		using namespace worldgen::ynd;
		std::vector<std::array<float, 3>> corners;
		{
			std::istringstream in(g_options["path"]);
			for (std::string item; std::getline(in, item, '/');)
			{
				std::array<float, 3> v{};
				char c1 = 0, c2 = 0;
				std::istringstream(item) >> v[0] >> c1 >> v[1] >> c2 >> v[2];
				corners.push_back(v);
			}
		}
		if (corners.size() < 2)
			return Fail("--path needs two points or more");
		const float spacing = g_options.contains("spacing") ? std::stof(g_options["spacing"]) : 20.0f;
		// lanes each way: a link's third flag byte holds the lanes along it (bits 5-7) and against it (bits 2-4)
		const int lanes = std::clamp(g_options.contains("lanes") ? std::stoi(g_options["lanes"]) : 1, 1, 7);
		std::string error;
		// the template: a vehicle node with two links. Either the first of nodes463 whose links have the same lanes and flags
		// (a plain two-way street), or (--template x,y) the one nearest to that place in the game's network whose links go
		// on in opposite directions (a street going straight through): its link towards one side is copied as each node's
		// link to the next one, the other as its link to the previous one
		std::vector<Loaded> all;
		if (!LoadAll(files, all, error))
			return Fail(error);
		const auto find = [&](const Link& l) -> const Node* {
			for (const Loaded& r : all)
				if (r.file == l.area && l.node < r.region.nodes.size())
					return &r.region.nodes[l.node];
			return nullptr;
		};
		const Node* tn = nullptr;
		Link back{}, ahead{};
		float best = 1e30f, wantX = 0, wantY = 0;
		const bool nearest = g_options.contains("template");
		if (nearest)
		{
			char c = 0;
			std::istringstream(g_options["template"]) >> wantX >> c >> wantY;
		}
		for (const Loaded& l : all)
		{
			if (l.file >= 1024 || (!nearest && l.file != 463))
				continue;
			for (uint32_t i = 0; i < l.region.vehicleNodes; ++i)
			{
				const Node& n = l.region.nodes[i];
				if (n.LinkCount() != 2)
					continue;
				const Link& l0 = l.region.links[n.FirstLink()];
				const Link& l1 = l.region.links[n.FirstLink() + 1];
				if (!nearest)
				{
					if (std::memcmp(l0.flags, l1.flags, 3) != 0 || l0.flags[2] != 0x24)
						continue;
					tn = &n, back = l0, ahead = l1;
					break;
				}
				const Node* a = find(l0);
				const Node* b = find(l1);
				if (!a || !b)
					continue;
				const auto f = [](int v) { return static_cast<float>(v); };
				const float ax = f(a->X() - n.X()), ay = f(a->Y() - n.Y()), bx = f(b->X() - n.X()), by = f(b->Y() - n.Y());
				if (ax * bx + ay * by > -0.95f * std::hypot(ax, ay) * std::hypot(bx, by))
					continue; // not straight through
				const float d = std::hypot(n.X() / 4.0f - wantX, n.Y() / 4.0f - wantY);
				if (d < best)
					best = d, tn = &n, back = l0, ahead = l1;
			}
			if (tn && !nearest)
				break;
		}
		if (!tn)
			return Fail("no template node");
		std::printf("template node at %.2f, %.2f: bytes +0x14 %02x%02x%02x%02x +0x20 %02x %02x +0x24 %02x %02x %02x %02x; links: back %02x %02x %02x, ahead %02x %02x %02x\n",
		    tn->X() / 4.0f, tn->Y() / 4.0f, tn->raw[0x14], tn->raw[0x15], tn->raw[0x16], tn->raw[0x17], tn->raw[0x20], tn->raw[0x21], tn->raw[0x24],
		    tn->raw[0x25], tn->raw[0x26], tn->raw[0x27], back.flags[0], back.flags[1], back.flags[2], ahead.flags[0], ahead.flags[1], ahead.flags[2]);

		// the points along the road, in order (each straight from its first corner; the last corner too)
		std::vector<std::array<float, 3>> points;
		for (size_t c = 0; c + 1 < corners.size(); ++c)
		{
			const auto& a = corners[c];
			const auto& b = corners[c + 1];
			const int steps = std::max(1, static_cast<int>(std::lround(std::hypot(b[0] - a[0], b[1] - a[1], b[2] - a[2]) / spacing)));
			for (int i = 0; i < steps; ++i)
			{
				const float t = static_cast<float>(i) / steps;
				points.push_back({a[0] + (b[0] - a[0]) * t, a[1] + (b[1] - a[1]) * t, a[2] + (b[2] - a[2]) * t});
			}
		}
		points.push_back(corners.back());
		const int count = static_cast<int>(points.size());

		// each point's region, and its place there: a region's vehicle nodes are in y order
		struct Place
		{
			uint32_t area, index;
		};
		std::vector<Place> places(count);
		std::map<uint32_t, std::vector<int>> members;
		for (int i = 0; i < count; ++i)
		{
			const auto& p = points[i];
			const int cx = static_cast<int>(std::floor((p[0] + 16384) / 1024)), cy = static_cast<int>(std::floor((p[1] + 16384) / 1024));
			places[i].area = static_cast<uint32_t>(cy * 32 + cx);
			members[places[i].area].push_back(i);
		}
		std::map<uint32_t, Region> regions;
		for (auto& [area, list] : members)
		{
			std::stable_sort(list.begin(), list.end(), [&](int a, int b) { return points[a][1] < points[b][1]; });
			for (size_t k = 0; k < list.size(); ++k)
				places[list[k]].index = static_cast<uint32_t>(k);
			regions[area].nodes.resize(list.size());
		}
		for (auto& [area, list] : members)
		{
			Region& r = regions[area];
			int ox, oy;
			Origin(area, ox, oy);
			for (const int i : list)
			{
				Node n = *tn;
				const int16_t x = static_cast<int16_t>(std::lround(points[i][0] * 4) - ox), y = static_cast<int16_t>(std::lround(points[i][1] * 4) - oy);
				const int16_t z = static_cast<int16_t>(std::lround(points[i][2] * 32));
				n.SetArea(static_cast<uint16_t>(area));
				n.SetIndex(static_cast<uint16_t>(places[i].index));
				n.SetFirstLink(static_cast<uint16_t>(r.links.size()));
				std::memcpy(n.raw + 0x1C, &x, 2);
				std::memcpy(n.raw + 0x1E, &y, 2);
				std::memcpy(n.raw + 0x22, &z, 2);
				std::memset(n.raw + 0x14, 0, 4); // no street name
				n.raw[0x24] &= 0x7F;               // switched on (bit 7: off, as the template was)
				uint8_t links = 0;
				for (const int j : {i - 1, i + 1})
				{
					if (j < 0 || j >= count)
						continue;
					Link l = j > i ? ahead : back;
					l.flags[2] = static_cast<uint8_t>((l.flags[2] & 3) | lanes << 5 | lanes << 2);
					l.area = static_cast<uint16_t>(places[j].area);
					l.node = static_cast<uint16_t>(places[j].index);
					l.flags[3] = static_cast<uint8_t>(std::min(255.0f, std::round(std::hypot(points[j][0] - points[i][0], points[j][1] - points[i][1],
					                                                                      points[j][2] - points[i][2]))));
					r.links.push_back(l);
					++links;
				}
				n.raw[0x25] = static_cast<uint8_t>((n.raw[0x25] & 7) | (links << 3));
				r.nodes[places[i].index] = n;
				++r.vehicleNodes;
			}
		}
		const fs::path out = g_options["out"];
		fs::create_directories(out);
		for (auto& [area, r] : regions)
		{
			Bytes file;
			if (!Write(r, file, error))
				return Fail(error);
			std::ofstream(out / ("nodes" + std::to_string(area) + ".ynd"), std::ios::binary).write(reinterpret_cast<const char*>(file.data()), file.size());
			int ox, oy;
			Origin(area, ox, oy);
			std::printf("nodes%u: %zu nodes, %zu links (origin %d, %d units)\n", area, r.nodes.size(), r.links.size(), ox, oy);
		}
		return 0;
	}

	int RegridAll(const ue5export::GameFiles& files)
	{
		std::vector<Loaded> all;
		std::string error;
		if (!LoadAll(files, all, error))
			return Fail(error);
		const fs::path out = g_options["out"];
		fs::create_directories(out);
		std::vector<Loaded> base, island;
		for (Loaded& l : all)
			(l.file < 1024 ? base : island).push_back(std::move(l));
		std::set<uint32_t> written, old;
		for (const Loaded& l : base)
			old.insert(l.file);
		for (const Loaded& l : island)
			old.insert(l.file);
		if (!Regrid(base, 0, out, written, error) || !Regrid(island, 1024, out, written, error))
			return Fail(error);
		// an old name with no nodes now: an empty region over it
		worldgen::ynd::Region empty;
		Bytes data;
		if (!worldgen::ynd::Write(empty, data, error))
			return Fail(error);
		size_t emptied = 0;
		for (const uint32_t file : old)
			if (!written.count(file))
			{
				std::ofstream(out / ("nodes" + std::to_string(file) + ".ynd"), std::ios::binary).write(reinterpret_cast<const char*>(data.data()), data.size());
				++emptied;
			}
		std::printf("%zu regions written, %zu old names emptied (%zu base + %zu Cayo Perico files read)\n", written.size(), emptied, base.size(),
		    island.size());
		return 0;
	}
}

namespace
{
	// Buildings of our own for checking, without the game: --world <world.json> --scan <rasters> --area x0,y0,x1,y1 --out
	// <file.obj> [--datum m] [--facades <dir>: the wall textures as pictures]
	int Shapes()
	{
		std::string error;
		if (g_options.contains("facades"))
		{
			const auto set = worldgen::facades::Make("wb_f_", 1024);
			fs::create_directories(g_options["facades"]);
			for (const auto& t : set.textures)
			{
				auto format = t.format;
				uint8_t mips = t.mips;
				auto pixels = t.pixels;
				loader::convert::MakeRenderTargetFormat(format, mips, t.width, t.height, pixels);
				worldgen::texture::Image img(t.width, t.height);
				for (size_t i = 0; i < img.pixels.size(); ++i)
					img.pixels[i] = {pixels[4 * i + 2], pixels[4 * i + 1], pixels[4 * i], 255};
				if (!worldgen::texture::WritePng(fs::path(g_options["facades"]) / (t.name + ".png"), img, error))
					return Fail(error);
			}
			std::printf("%zu textures in %s\n", set.textures.size(), g_options["facades"].c_str());
			if (!g_options.contains("world"))
				return 0;
		}
		if (!g_options.contains("world") || !g_options.contains("scan") || !g_options.contains("area") || !g_options.contains("out"))
			return Fail("--world <world.json> --scan <dir> --area x0,y0,x1,y1 --out <file.obj> are needed");
		worldgen::world::World w;
		if (!worldgen::world::Load(g_options["world"], w, error))
			return Fail(error);
		worldgen::elevation::Scan scan;
		if (!scan.Open(g_options["scan"], g_options.contains("datum") ? std::stof(g_options["datum"]) : worldgen::build::Options{}.datum))
			return Fail("no scan rasters in " + g_options["scan"]);
		float a[4] = {};
		char c = 0;
		std::istringstream(g_options["area"]) >> a[0] >> c >> a[1] >> c >> a[2] >> c >> a[3];
		const auto ground = [&](float x, float y) {
			for (float r = 0; r <= 8; r += 1)
				for (int k = 0; k < (r == 0 ? 1 : 8); ++k)
				{
					const float g = scan.Ground(x + r * std::cos(k * 0.785f), y + r * std::sin(k * 0.785f));
					if (!std::isnan(g))
						return g;
				}
			return 1.5f;
		};
		worldgen::structures::Options so;
		worldgen::structures::Stats ss;
		std::ofstream obj(g_options["out"]);
		size_t base = 1, count = 0;
		const auto start = std::chrono::steady_clock::now();
		for (const auto& b : w.buildings)
		{
			if (b.footprint.size() < 3)
				continue;
			float cx = 0, cy = 0;
			for (const auto& q : b.footprint)
				cx += q[0] / b.footprint.size(), cy += q[1] / b.footprint.size();
			if (cx < a[0] || cx >= a[2] || cy < a[1] || cy >= a[3])
				continue;
			worldgen::structures::Shape s;
			if (!worldgen::structures::Make(b, &scan, ground, so, s, ss))
				continue;
			++count;
			obj << "o b" << count << "\n";
			for (const auto& v : s.vertices)
				obj << std::format("v {:.3f} {:.3f} {:.3f}\n", v[0], v[2], -v[1]);
			for (const auto& v : s.vertices)
				obj << std::format("v {:.3f} {:.3f} {:.3f}\n", v[0], s.base - 0.6, -v[1]);
			const size_t n = s.vertices.size();
			for (const auto& t : s.roof)
				obj << "f " << base + t[0] << ' ' << base + t[1] << ' ' << base + t[2] << "\n";
			for (const auto& e : s.outline)
				obj << "f " << base + n + e[0] << ' ' << base + n + e[1] << ' ' << base + e[1] << ' ' << base + e[0] << "\n";
			base += 2 * n;
		}
		std::printf("%zu buildings (%zu measured: %zu flat, %zu shaped; %zu at the map's height; %zu skipped), %zu roof triangles, %.1f s\n", count,
		    ss.measured, ss.flat, ss.shaped, ss.mapHeight, ss.skipped, ss.triangles,
		    std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count());
		return 0;
	}
	// One DLC pack of these files (one archive: the offsets in its table count 512-byte blocks in 23 bits, 4 GB) at `out`.
	// Its streaming archives are plain ones (not overlays): files the game does not have are registered as new (a map is
	// then loaded where it is). A plain archive's file with a name the game has (`gameHas`: the road network's regions,
	// navmeshes where the game has its own) is refused, the game's kept - those go in an overlay archive, on from the
	// start (research/phase0.md §61.27). `parentTxds` (gtxd.meta) and `requestsFile` (the game's archetype files to keep
	// loaded) for the first pack only.
	bool WritePack(const std::string& name, std::vector<loader::convert::WriteNode> files, const std::string& parentTxds,
	    const fs::path& requestsFile, const std::function<bool(const std::string&)>& gameHas, const fs::path& out, std::string& error)
	{
		const std::string device = "dlc_" + name;
		const size_t count = files.size();
		std::vector<loader::convert::WriteNode> replacing;
		for (auto it = files.begin(); it != files.end();)
			if (gameHas(it->name))
				replacing.push_back(std::move(*it)), it = files.erase(it);
			else
				++it;
		const std::string startupSet = name + "_STARTUP";
		// The archive is a map change set of story mode (GROUP_MAP_SP), as the game's own DLC maps are: the game then reads
		// the pack's map cache (x64\data\cacheloaderdata_dlc\<pack>_<change set hash>_cache_y.dat) for where each map and
		// collision file lies. Without it a new map is registered disabled and without its box, and never loaded.
		const std::string changeSet = name + "_MAP";
		// The streaming files in archives of at most 1800 files and 48 KB of names (an archive's name table is
		// addressed with 16 bits): <name>.rpf, <name>_1.rpf, ...
		std::vector<std::vector<size_t>> groups(1);
		std::vector<std::string> archiveNames{name};
		std::map<std::string, std::string> archiveOf;
		{
			size_t names = 0;
			for (size_t i = 0; i < files.size(); ++i)
			{
				const size_t length = files[i].name.size() + 1;
				if (groups.back().size() >= 1800 || names + length > 48000)
				{
					groups.emplace_back();
					archiveNames.push_back(std::format("{}_{}", name, groups.size() - 1));
					names = 0;
				}
				groups.back().push_back(i);
				names += length;
				archiveOf[files[i].name] = archiveNames.back();
			}
		}
		// The map change sets: the archives in sets whose map cache (64 bytes a map, 32 a collision file) stays under 240 KB
		// - the game's reader of a DLC's cache stops at 256 KB (0xCE07B0: 0x40000, an int3 past it; a dense sector came to
		// 267 KB): <name>_MAP, <name>_MAP2, ..., all of GROUP_MAP_SP
		std::vector<int> setOf(archiveNames.size(), 0);
		std::vector<std::string> setNames{changeSet};
		{
			std::map<std::string, size_t> cacheBytes;
			for (const auto& f : files)
				cacheBytes[archiveOf[f.name]] += f.name.ends_with(".ymap") ? 64 : f.name.ends_with(".ybn") ? 32 : 0;
			size_t used = 0;
			for (size_t g = 0; g < archiveNames.size(); ++g)
			{
				const size_t bytes = cacheBytes[archiveNames[g]];
				if (used && used + bytes > 240000)
				{
					setNames.push_back(std::format("{}{}", changeSet, setNames.size() + 1));
					used = 0;
				}
				setOf[g] = static_cast<int>(setNames.size() - 1);
				used += bytes;
			}
		}
		std::string rpfItems;
		std::vector<std::string> rpfEnables(setNames.size());
		for (size_t g = 0; g < archiveNames.size(); ++g)
		{
			const std::string& a = archiveNames[g];
			rpfItems += std::format("    <Item>\n      <filename>{}:/%PLATFORM%/levels/gta5/{}.rpf</filename>\n      <fileType>RPF_FILE</fileType>\n"
			                        "      <overlay value=\"false\" />\n      <disabled value=\"true\" />\n      <persistent value=\"true\" />\n"
			                        "      <contents>CONTENTS_DLC_MAP_DATA</contents>\n    </Item>\n",
			    device, a);
			rpfEnables[setOf[g]] += std::format("            <Item>{}:/%PLATFORM%/levels/gta5/{}.rpf</Item>\n", device, a);
		}
		// (the files replacing the game's, and the navmeshes: overlay archives, the start-up change set's - as the others,
		// at most 1800 files and 48 KB of names each: <name>_over.rpf, <name>_over_1.rpf, ...)
		std::string startupSets, startupGroup;
		std::vector<std::vector<size_t>> overlayGroups;
		std::vector<std::string> overlayNames;
		{
			size_t names = 0;
			for (size_t i = 0; i < replacing.size(); ++i)
			{
				const size_t length = replacing[i].name.size() + 1;
				if (overlayGroups.empty() || overlayGroups.back().size() >= 1800 || names + length > 48000)
				{
					overlayGroups.emplace_back();
					overlayNames.push_back(overlayNames.empty() ? name + "_over" : std::format("{}_over_{}", name, overlayNames.size()));
					names = 0;
				}
				overlayGroups.back().push_back(i);
				names += length;
			}
		}
		if (!replacing.empty())
		{
			std::string enables;
			for (const std::string& a : overlayNames)
			{
				rpfItems += std::format("    <Item>\n      <filename>{}:/%PLATFORM%/levels/gta5/{}.rpf</filename>\n      <fileType>RPF_FILE</fileType>\n"
				                        "      <overlay value=\"true\" />\n      <disabled value=\"true\" />\n      <persistent value=\"true\" />\n    </Item>\n",
				    device, a);
				enables += std::format("        <Item>{}:/%PLATFORM%/levels/gta5/{}.rpf</Item>\n", device, a);
			}
			startupSets = std::format("    <Item>\n      <changeSetName>{}</changeSetName>\n      <filesToDisable />\n      <filesToEnable>\n"
			                          "{}      </filesToEnable>\n      <txdToLoad />\n"
			                          "      <txdToUnload />\n      <residentResources />\n      <unregisterResources />\n    </Item>\n",
			    startupSet, enables);
			startupGroup = std::format("    <Item>\n      <NameHash>GROUP_STARTUP</NameHash>\n      <ContentChangeSets>\n        <Item>{}</Item>\n"
			                           "      </ContentChangeSets>\n    </Item>\n",
			    startupSet);
		}
		// Archetype files (.ytyp): requested with the change set that enables their archive (DLC_ITYP_REQUEST), kept while
		// it is on - asked for by an earlier change set, before their archive was there, they were never loaded: none of
		// the land's own models drawn (a town's pack, its archetype files past the first set's archives)
		std::string typeItems, typeEnables;
		std::vector<std::string> ownTypeEnables(setNames.size());
		{
			std::map<std::string, size_t> archiveIndex;
			for (size_t g = 0; g < archiveNames.size(); ++g)
				archiveIndex[archiveNames[g]] = g;
			for (const auto& child : files)
				if (child.name.ends_with(".ytyp"))
				{
					const std::string path = std::format("{}:/%PLATFORM%/levels/gta5/{}.rpf/{}", device, archiveOf[child.name], child.name);
					typeItems += std::format("    <Item>\n      <filename>{}</filename>\n      <fileType>DLC_ITYP_REQUEST</fileType>\n"
					                         "      <overlay value=\"false\" />\n      <disabled value=\"true\" />\n      <persistent value=\"true\" />\n    </Item>\n",
					    path);
					ownTypeEnables[setOf[archiveIndex.at(archiveOf[child.name])]] += std::format("            <Item>{}</Item>\n", path);
				}
		}
		// (requests.txt in the input: the game's own archetype files to keep loaded, one path a line, "platform:/..." -
		// those of the game's models the world places)
		{
			std::ifstream requests(requestsFile);
			for (std::string path; std::getline(requests, path);)
			{
				while (!path.empty() && (path.back() == '\r' || path.back() == ' '))
					path.pop_back();
				if (path.empty() || path[0] == '#')
					continue;
				typeItems += std::format("    <Item>\n      <filename>{}</filename>\n      <fileType>DLC_ITYP_REQUEST</fileType>\n"
				                         "      <overlay value=\"false\" />\n      <disabled value=\"true\" />\n      <persistent value=\"true\" />\n    </Item>\n",
				    path);
				typeEnables += std::format("            <Item>{}</Item>\n", path);
			}
		}
		// texture dictionaries' parents (gtxd.meta in the input): GTXD_PARENTING_DATA, on with the change set
		if (!parentTxds.empty())
		{
			typeItems += std::format("    <Item>\n      <filename>{}:/common/data/gtxd.meta</filename>\n      <fileType>GTXD_PARENTING_DATA</fileType>\n"
			                         "      <overlay value=\"false\" />\n      <disabled value=\"true\" />\n      <persistent value=\"false\" />\n    </Item>\n",
			    device);
			typeEnables += std::format("            <Item>{}:/common/data/gtxd.meta</Item>\n", device);
		}
		// (the game's archetype files and the texture parents with the first map change set, ours with their archives')
		std::string mapSets, mapSetNames;
		for (size_t s = 0; s < setNames.size(); ++s)
		{
			mapSets += std::format("    <Item>\n      <changeSetName>{}</changeSetName>\n      <mapChangeSetData>\n        <Item>\n"
			                       "          <associatedMap>MO_JIM_L11</associatedMap>\n          <filesToInvalidate />\n          <filesToEnable>\n"
			                       "{}{}          </filesToEnable>\n        </Item>\n      </mapChangeSetData>\n"
			                       "      <requiresLoadingScreen value=\"true\" />\n      <loadingScreenContext>LOADINGSCREEN_CONTEXT_LAST_FRAME</loadingScreenContext>\n"
			                       "      <useCacheLoader value=\"true\" />\n    </Item>\n",
			    setNames[s], rpfEnables[s], ownTypeEnables[s] + (s ? std::string() : typeEnables));
			mapSetNames += std::format("        <Item>{}</Item>\n", setNames[s]);
		}
		const std::string content = std::format(
		    "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n<CDataFileMgr__ContentsOfDataFileXml>\n  <disabledFiles />\n  <includedXmlFiles />\n"
		    "  <includedDataFiles />\n  <dataFiles>\n{}{}  </dataFiles>\n  <contentChangeSets>\n{}{}  </contentChangeSets>\n  <patchFiles />\n"
		    "</CDataFileMgr__ContentsOfDataFileXml>\n",
		    rpfItems, typeItems, mapSets, startupSets);
		const std::string setup = std::format(
		    "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n<SSetupData>\n  <deviceName>{}</deviceName>\n  <datFile>content.xml</datFile>\n"
		    "  <timeStamp>00/00/0000 00:00:00</timeStamp>\n  <nameHash>{}</nameHash>\n  <contentChangeSets />\n  <contentChangeSetGroups>\n"
		    "{}    <Item>\n      <NameHash>GROUP_MAP_SP</NameHash>\n      <ContentChangeSets>\n{}      </ContentChangeSets>\n"
		    "    </Item>\n  </contentChangeSetGroups>\n  <type>EXTRACONTENT_COMPAT_PACK</type>\n  <order value=\"999\" />\n</SSetupData>\n",
		    device, name, startupGroup, mapSetNames);
		// The cache (version 46): a header, <fileDates> (each archive: hash of its path, a file time), then per module its
		// entries after their byte count. fwMapDataStore, 64 bytes a map: name, parent, content flags, streaming box, entities
		// box, flags (1: streamed like the game's own maps). BoundsStore, 32 bytes a collision file: name, box, flags (0 a map's
		// collision, 1 its high detail one - hi@ -, 2 a material bound - ma@: only these the procedural plants are looked for in).
		// (one a map change set: its archives' maps and collision files)
		std::map<std::string, int> setOfArchive;
		for (size_t g = 0; g < archiveNames.size(); ++g)
			setOfArchive[archiveNames[g]] = setOf[g];
		std::vector<Bytes> setMaps(setNames.size()), setBounds(setNames.size());
		const auto put32 = [](Bytes& b, uint32_t v) { b.insert(b.end(), reinterpret_cast<uint8_t*>(&v), reinterpret_cast<uint8_t*>(&v) + 4); };
		const auto putf = [](Bytes& b, float v) { b.insert(b.end(), reinterpret_cast<uint8_t*>(&v), reinterpret_cast<uint8_t*>(&v) + 4); };
		for (const auto& child : files)
		{
			const std::string base = child.name.substr(0, child.name.rfind('.'));
			const int set = setOfArchive[archiveOf[child.name]];
			Bytes& maps = setMaps[set];
			Bytes& bounds = setBounds[set];
			if (child.name.ends_with(".ymap"))
			{
				ue5export::meta::Meta mm;
				if (!mm.Read(child.data, error))
					return error = child.name + ": " + error, false;
				using ue5export::meta::Hash;
				const auto r = mm.Root();
				put32(maps, loader::convert::Joaat(base));
				put32(maps, r.Get<uint32_t>(Hash("parent")));
				put32(maps, r.Get<uint32_t>(Hash("contentFlags")));
				for (const char* field : {"streamingExtentsMin", "streamingExtentsMax", "entitiesExtentsMin", "entitiesExtentsMax"})
				{
					const auto v = r.Get<std::array<float, 4>>(Hash(field));
					for (int a = 0; a < 3; ++a)
						putf(maps, v[a]);
				}
				put32(maps, 1); // (always loaded where it is; 0: an IPL group's, off until a script asks for it)
			}
			else if (child.name.ends_with(".ybn"))
			{
				loader::convert::Resource res;
				if (!loader::convert::Read(child.data, res, error))
					return error = child.name + ": " + error, false;
				const ue5export::drawable::View view(res);
				put32(bounds, loader::convert::Joaat(base));
				for (const uint64_t at : {ue5export::drawable::View::kRoot + 0x30, ue5export::drawable::View::kRoot + 0x20})
					for (int a = 0; a < 3; ++a)
						putf(bounds, view.Get<float>(at + 4 * a));
				put32(bounds, base.starts_with("ma@") ? 2 : base.starts_with("hi@") ? 1 : 0);
			}
		}
		std::vector<Bytes> caches;
		for (size_t s = 0; s < setNames.size(); ++s)
		{
			Bytes cache(0x64, 0);
			const std::string head = "[VERSION]\n46\n";
			std::memcpy(cache.data(), head.data(), head.size());
			const auto append = [&](const std::string& t) { cache.insert(cache.end(), t.begin(), t.end()); };
			append("<fileDates>\n</fileDates>\n");
			for (const auto& [module, payload] :
			    {std::pair<const char*, const Bytes*>{"fwMapDataStore", &setMaps[s]}, {"CInteriorProxy", nullptr}, {"BoundsStore", &setBounds[s]}})
			{
				append(std::format("<module>\n{}\n", module));
				put32(cache, payload ? static_cast<uint32_t>(payload->size()) : 0);
				if (payload)
					cache.insert(cache.end(), payload->begin(), payload->end());
				append("</module>\n");
			}
			caches.push_back(std::move(cache));
		}
		loader::convert::WriteNode root, platform;
		const auto text = [](const char* file, const std::string& s) {
			loader::convert::WriteNode n;
			n.name = file;
			n.kind = loader::convert::WriteNode::Kind::File;
			n.data.assign(s.begin(), s.end());
			return n;
		};
		root.children.push_back(text("content.xml", content));
		root.children.push_back(text("setup2.xml", setup));
		if (!parentTxds.empty())
		{
			loader::convert::WriteNode common, commonData;
			common.name = "common";
			commonData.name = "data";
			commonData.children.push_back(text("gtxd.meta", parentTxds));
			common.children.push_back(std::move(commonData));
			root.children.push_back(std::move(common));
		}
		std::string lowerName = name;
		std::transform(lowerName.begin(), lowerName.end(), lowerName.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
		loader::convert::WriteNode data, loaderData;
		data.name = "data";
		loaderData.name = "cacheloaderdata_dlc";
		for (size_t s = 0; s < setNames.size(); ++s)
		{
			loader::convert::WriteNode cacheFile;
			cacheFile.name = std::format("{}_{}_cache_y.dat", lowerName, loader::convert::Joaat(setNames[s]));
			cacheFile.kind = loader::convert::WriteNode::Kind::File;
			cacheFile.data = std::move(caches[s]);
			loaderData.children.push_back(std::move(cacheFile));
		}
		data.children.push_back(std::move(loaderData));
		platform.children.push_back(std::move(data));
		// (where the game's own map archives are: x64\levels\gta5)
		loader::convert::WriteNode levels, gta5;
		levels.name = "levels";
		gta5.name = "gta5";
		for (size_t g = 0; g < groups.size(); ++g)
		{
			loader::convert::WriteNode a;
			a.name = archiveNames[g] + ".rpf";
			a.kind = loader::convert::WriteNode::Kind::Archive;
			for (const size_t i : groups[g])
				a.children.push_back(std::move(files[i]));
			gta5.children.push_back(std::move(a));
		}
		for (size_t g = 0; g < overlayGroups.size(); ++g)
		{
			loader::convert::WriteNode a;
			a.name = overlayNames[g] + ".rpf";
			a.kind = loader::convert::WriteNode::Kind::Archive;
			for (const size_t i : overlayGroups[g])
				a.children.push_back(std::move(replacing[i]));
			gta5.children.push_back(std::move(a));
		}
		levels.children.push_back(std::move(gta5));
		platform.name = "x64";
		platform.children.push_back(std::move(levels));
		root.children.push_back(std::move(platform));
		Bytes packed;
		if (!loader::convert::BuildArchive(root, packed, error))
			return false;
		fs::create_directories(out.parent_path());
		// (written beside it and put in its place whole: a game starting meanwhile took a half-written pack - the loader
		// replaces dlc.rpf with a dlc.rpf.new it finds)
		fs::path part = out;
		part += ".part";
		{
			std::ofstream file(part, std::ios::binary);
			file.write(reinterpret_cast<const char*>(packed.data()), static_cast<std::streamsize>(packed.size()));
			if (!file.flush())
				return error = "cannot write " + part.string(), false;
		}
		std::error_code ec;
		fs::rename(part, out, ec);
		if (ec)
			return error = "cannot put " + part.string() + " in place: " + ec.message(), false;
		std::printf("%s: %zu files in %zu archive(s), %zu map change set(s) (%zu bytes)\n", out.string().c_str(), count, archiveNames.size(),
		    setNames.size(), packed.size());
		return true;
	}
}

int main(int argc, char** argv)
{
	_setmaxstdio(8192);
	// (unbuffered: a long build's progress shows as it happens, also into a log)
	std::setvbuf(stdout, nullptr, _IONBF, 0);
	// A batch's steps (sectors.py sets WG_BACKGROUND) in Windows' background mode - idle CPU, very low I/O and memory
	// priority: the game played meanwhile reads from the disk the batch writes.
	if (const char* background = std::getenv("WG_BACKGROUND"); background && *background == '1')
		SetPriorityClass(GetCurrentProcess(), PROCESS_MODE_BACKGROUND_BEGIN);
	if (argc < 2)
		return Fail("usage: worldgen roundtrip|extract-ynd|regrid|testroad --game <dir> --keys <ng.bin> [--out <dir>]");
	for (int i = 2; i + 1 < argc; i += 2)
		if (std::string(argv[i]).starts_with("--"))
			g_options[argv[i] + 2] = argv[i + 1];
	if (std::string(argv[1]) == "shapes")
		return Shapes();
	if (std::string(argv[1]) == "edit")
	{
		// a road edit as the world editor makes it, for checking: --roads <roads.bin> --edits <edits.json> then
		// --road <type id> --points x,y;x,y;... [--curve 1: start;control;end] or --delete x,y; the edits file is updated
		worldgen::roadedit::Network net;
		std::string error;
		if (!net.Load(g_options["roads"], g_options["edits"], error))
			return Fail(error);
		const auto point = [](const std::string& s) {
			worldgen::world::P2 p{};
			char c = 0;
			std::istringstream(s) >> p[0] >> c >> p[1];
			return p;
		};
		worldgen::roadedit::Change change;
		if (g_options.contains("road"))
		{
			const auto& types = worldgen::roadedit::Types();
			const auto type = std::find_if(types.begin(), types.end(), [&](const auto& t) { return g_options["road"] == t.id; });
			if (type == types.end())
				return Fail("unknown road type " + g_options["road"]);
			std::vector<worldgen::world::P2> pts;
			std::istringstream list(g_options["points"]);
			for (std::string item; std::getline(list, item, ';');)
				pts.push_back(point(item));
			if (pts.size() < 2)
				return Fail("--points x,y;x,y... (two at least)");
			if (g_options.contains("curve") && pts.size() == 3)
				pts = worldgen::roadedit::Network::Curve(pts[0], pts[1], pts[2]);
			const auto start = net.SnapTo(pts.front(), 8), end = net.SnapTo(pts.back(), 8);
			change = net.Plan(pts, *type, start, end);
		}
		else if (g_options.contains("delete"))
			change = net.PlanDelete(point(g_options["delete"]), 8);
		else
			return Fail("--road <type> --points ... or --delete x,y");
		for (const auto& r : change.roads)
			std::printf("road %lld (%s, %d+%d): %zu points%s\n", static_cast<long long>(r.id), r.kind.c_str(), r.forward, r.backward, r.points.size(),
			    net.Find(r.id) ? " (changed)" : " (new)");
		for (const int64_t id : change.removed)
			std::printf("road %lld removed\n", static_cast<long long>(id));
		net.Commit(change);
		if (!net.SaveEdits(g_options["edits"], error))
			return Fail(error);
		std::printf("%zu edits in %s\n", net.Edits(), g_options["edits"].c_str());
		return 0;
	}
	if (std::string(argv[1]) == "roads-index")
	{
		// the roads for the world editor: --world <world.json> [--edits <edits.json>] --out <roads.bin>
		worldgen::world::World w;
		std::string error;
		if (!worldgen::world::Load(g_options["world"], w, error))
			return Fail(error);
		if (g_options.contains("edits") && !worldgen::world::ApplyEdits(w, g_options["edits"], error))
			return Fail(error);
		if (!worldgen::world::SaveRoadIndex(w, g_options["out"], error))
			return Fail(error);
		std::printf("%zu roads in %s\n", w.roads.size(), g_options["out"].c_str());
		return 0;
	}
	if (std::string(argv[1]) == "ytd-png")
	{
		// a texture dictionary's textures as pictures (mip 0; block formats BC1-3): --ytd <file> --out <dir>
		std::ifstream in(g_options["ytd"], std::ios::binary);
		Bytes data((std::istreambuf_iterator<char>(in)), {});
		loader::convert::Resource res;
		std::string error;
		if (!loader::convert::Read(data, res, error))
			return Fail(error);
		const ue5export::drawable::View view(res);
		fs::create_directories(g_options["out"]);
		for (const auto& t : ue5export::drawable::Textures(view, ue5export::drawable::View::kRoot))
		{
			std::printf("%s %ux%u format %u mips %u\n", t.name.c_str(), t.width, t.height, t.dxgi, t.mips);
			loader::convert::TextureFormat format{t.dxgi, t.dxgi == 71 ? 8u : 16u, true};
			uint8_t mips = t.mips;
			Bytes pixels(t.data.begin(), t.data.end());
			loader::convert::MakeRenderTargetFormat(format, mips, static_cast<uint16_t>(t.width), static_cast<uint16_t>(t.height), pixels);
			worldgen::texture::Image img(t.width, t.height), alpha(t.width, t.height);
			for (size_t i = 0; i < img.pixels.size() && 4 * i + 3 < pixels.size(); ++i)
			{
				img.pixels[i] = {pixels[4 * i + 2], pixels[4 * i + 1], pixels[4 * i], 255};
				alpha.pixels[i] = {pixels[4 * i + 3], pixels[4 * i + 3], pixels[4 * i + 3], 255};
			}
			if (!worldgen::texture::WritePng(fs::path(g_options["out"]) / (t.name + ".png"), img, error))
				return Fail(error);
			// (and its alpha, where it has one: BC2, BC3)
			if ((t.dxgi >= 74 && t.dxgi <= 79) && !worldgen::texture::WritePng(fs::path(g_options["out"]) / (t.name + "_alpha.png"), alpha, error))
				return Fail(error);
		}
		return 0;
	}
	if (!g_options.contains("game") || !g_options.contains("keys"))
		return Fail("--game <dir> and --keys <ng.bin> are needed");
	ue5export::ng::Keys keys;
	std::string error;
	if (!ue5export::ng::Load(g_options["keys"], keys, error))
		return Fail(error);
	const loader::convert::Decryptor decrypt = [&keys](uint32_t key, uint8_t* data, uint32_t size) { ue5export::ng::Decrypt(keys, key, data, size); };
	SetDllDirectoryW(fs::path(g_options["game"]).c_str()); // (the game's zlib1.dll)
	ue5export::GameFiles files;
	if (!files.Open(g_options["game"], decrypt, error))
		return Fail(error);
	const std::string command = argv[1];
	if (command == "roundtrip")
		return Roundtrip(files);
	if (command == "check-paths")
	{
		// every road-network region (those in --dir, or the ones the game uses) checked as the game's code takes them: each
		// node's own area and index, its links inside the file's table, each link's target node there (in its region's
		// file, its index under that file's count, its own area the link's) - a link that is not crashed the vehicle AI's
		// choice of the next node (research/phase0.md §61.27)
		std::map<uint32_t, worldgen::ynd::Region> regions;
		for (uint32_t i = 0; i < 2048; ++i)
		{
			Bytes data;
			if (g_options.contains("dir"))
			{
				const fs::path f = fs::path(g_options["dir"]) / ("nodes" + std::to_string(i) + ".ynd");
				if (!fs::exists(f))
					continue;
				std::ifstream in(f, std::ios::binary);
				data.assign(std::istreambuf_iterator<char>(in), {});
			}
			else if (const auto* f = files.Find("nodes" + std::to_string(i) + ".ynd"))
			{
				if (!files.Read(*f, data, error))
					return Fail(error);
			}
			else
				continue;
			worldgen::ynd::Region r;
			if (!worldgen::ynd::Read(data, r, error))
				return Fail(std::format("nodes{}.ynd: {}", i, error));
			regions.emplace(i, std::move(r));
		}
		size_t nodes = 0, links = 0, bad = 0;
		const auto report = [&](const std::string& what) {
			if (++bad <= 40)
				std::printf("%s\n", what.c_str());
		};
		for (const auto& [area, r] : regions)
			for (size_t k = 0; k < r.nodes.size(); ++k)
			{
				const auto& n = r.nodes[k];
				++nodes;
				if (n.Area() != area || n.Index() != k)
					report(std::format("nodes{}.ynd node {}: says it is area {} node {}", area, k, n.Area(), n.Index()));
				if (n.FirstLink() + n.LinkCount() > r.links.size())
					report(std::format("nodes{}.ynd node {}: links {}+{} past the table ({})", area, k, n.FirstLink(), n.LinkCount(), r.links.size()));
				for (uint32_t l = 0; l < n.LinkCount() && n.FirstLink() + l < r.links.size(); ++l)
				{
					const auto& link = r.links[n.FirstLink() + l];
					++links;
					const auto t = regions.find(link.area);
					if (t == regions.end())
						report(std::format("nodes{}.ynd node {} ({:.1f}, {:.1f}): link to area {} (no file)", area, k, n.X() / 4.0, n.Y() / 4.0, link.area));
					else if (link.node >= t->second.nodes.size())
						report(std::format("nodes{}.ynd node {}: link to area {} node {} (it has {})", area, k, link.area, link.node, t->second.nodes.size()));
					else if (t->second.nodes[link.node].Area() != link.area)
						report(std::format("nodes{}.ynd node {}: link to area {} node {}, which says area {}", area, k, link.area, link.node,
						    t->second.nodes[link.node].Area()));
				}
			}
		std::printf("%zu regions, %zu nodes, %zu links: %zu wrong\n", regions.size(), nodes, links, bad);
		{
			// (junction nodes - +0x24 bit 2 - and the junction records (height maps) their regions name them in; traffic
			// lights: +0x21 bits 3-7 = 15, the nodes cars stop at before a junction)
			size_t junctionNodes = 0, withRecord = 0, lights = 0;
			for (const auto& [area, r] : regions)
			{
				std::set<uint16_t> referenced;
				for (const auto& ref : r.refs)
					referenced.insert(ref.node);
				for (size_t k = 0; k < r.nodes.size(); ++k)
				{
					if (r.nodes[k].raw[0x24] & 0x04)
						++junctionNodes, withRecord += referenced.contains(static_cast<uint16_t>(k));
					lights += (r.nodes[k].raw[0x21] >> 3) == 15;
				}
			}
			std::printf("%zu junction nodes (%zu with a junction record), %zu traffic light nodes\n", junctionNodes, withRecord, lights);
			// (the junctions the traffic light nodes lead into: the nodes they link to that are junctions - with a record?)
			size_t intoJunction = 0, intoRecorded = 0;
			std::map<int, size_t> linkFlags1;
			for (const auto& [area, r] : regions)
				for (size_t k = 0; k < r.nodes.size(); ++k)
				{
					const auto& nd = r.nodes[k];
					if ((nd.raw[0x21] >> 3) != 15)
						continue;
					bool junction = false, recorded = false;
					for (uint32_t l = 0; l < nd.LinkCount() && nd.FirstLink() + l < r.links.size(); ++l)
					{
						const auto& link = r.links[nd.FirstLink() + l];
						const auto t = regions.find(link.area);
						if (t == regions.end() || link.node >= t->second.nodes.size())
							continue;
						if (t->second.nodes[link.node].raw[0x24] & 0x04)
						{
							junction = true;
							++linkFlags1[link.flags[1]];
							for (const auto& ref : t->second.refs)
								recorded |= ref.node == link.node;
						}
					}
					intoJunction += junction, intoRecorded += recorded;
				}
			std::printf("traffic light nodes next to a junction node: %zu (%zu with a record)\n", intoJunction, intoRecorded);
			{
				// (how far before the junction node the cars stop: the light nodes' distances to the junction nodes they link to)
				std::vector<float> back;
				for (const auto& [area, r] : regions)
					for (const auto& nd : r.nodes)
						if ((nd.raw[0x21] >> 3) == 15)
							for (uint32_t l = 0; l < nd.LinkCount() && nd.FirstLink() + l < r.links.size(); ++l)
							{
								const auto& link = r.links[nd.FirstLink() + l];
								if (link.area != area || link.node >= r.nodes.size() || !(r.nodes[link.node].raw[0x24] & 0x04))
									continue;
								const auto& to = r.nodes[link.node];
								back.push_back(static_cast<float>(std::hypot(to.X() - nd.X(), to.Y() - nd.Y()) / 4.0));
							}
				std::sort(back.begin(), back.end());
				if (!back.empty())
					std::printf("  light node to junction node: %zu, 10%% %.1f m, 25%% %.1f, median %.1f, 75%% %.1f, 90%% %.1f\n", back.size(),
					    back[back.size() / 10], back[back.size() / 4], back[back.size() / 2], back[back.size() * 3 / 4], back[back.size() * 9 / 10]);
			}
			// (how the junction nodes' other bits differ from the rest: +0x25 bits 0-2, +0x20, +0x26 bit 7)
			std::map<std::string, size_t> bits;
			for (const auto& [area, r] : regions)
				for (const auto& nd : r.nodes)
				{
					const bool junction = nd.raw[0x24] & 0x04, light = (nd.raw[0x21] >> 3) == 15;
					const std::string kind = junction ? "junction" : light ? "light" : "plain";
					++bits[std::format("{} +25&7={}", kind, nd.raw[0x25] & 7)];
					++bits[std::format("{} +20={:02x}", kind, nd.raw[0x20])];
				}
			for (const auto& [k, c] : bits)
				if (c > 20)
					std::printf("  %s: %zu\n", k.c_str(), c);
			// (the links' third flag byte, bit 0, by what they join: light -> junction, junction -> anything, plain -> plain)
			std::map<std::string, std::array<size_t, 2>> linkBit;
			for (const auto& [area, r] : regions)
				for (const auto& nd : r.nodes)
					for (uint32_t l = 0; l < nd.LinkCount() && nd.FirstLink() + l < r.links.size(); ++l)
					{
						const auto& link = r.links[nd.FirstLink() + l];
						const auto t = regions.find(link.area);
						if (t == regions.end() || link.node >= t->second.nodes.size())
							continue;
						const auto& to = t->second.nodes[link.node];
						const auto kindOf = [](const worldgen::ynd::Node& x) {
							return (x.raw[0x24] & 0x04) ? "junction" : (x.raw[0x21] >> 3) == 15 ? "light" : "plain";
						};
						++linkBit[std::format("{} -> {}", kindOf(nd), kindOf(to))][link.flags[2] & 1];
					}
			for (const auto& [k, c] : linkBit)
				std::printf("  link %s: bit 0 clear %zu, set %zu\n", k.c_str(), c[0], c[1]);
		}
		if (g_options.contains("at"))
		{
			// (the nodes within --at x,y,r: position, link count, their flag bytes +0x20..+0x27, and their links' flags)
			float at[3]{};
			std::sscanf(g_options["at"].c_str(), "%f,%f,%f", &at[0], &at[1], &at[2]);
			for (const auto& [area, r] : regions)
			{
				if (area >= 1024)
					continue;
				const int cx = static_cast<int>(area % 32), cy = static_cast<int>(area / 32);
				const bool inside = cx >= 8 && cx < 24 && cy >= 8 && cy < 24;
				for (size_t k = 0; k < r.nodes.size(); ++k)
				{
					const auto& n = r.nodes[k];
					const double x = n.X() / 4.0 + (inside ? 0 : cx * 1024 - 16384), y = n.Y() / 4.0 + (inside ? 0 : cy * 1024 - 16384);
					if (std::hypot(x - at[0], y - at[1]) > at[2])
						continue;
					std::string line = std::format("area {} node {} ({:.1f}, {:.1f}) {} links, +0x20:", area, k, x, y, n.LinkCount());
					for (int b = 0x20; b < 0x28; ++b)
						line += std::format(" {:02x}", n.raw[b]);
					line += " | links:";
					for (uint32_t l = 0; l < n.LinkCount() && n.FirstLink() + l < r.links.size(); ++l)
					{
						const auto& link = r.links[n.FirstLink() + l];
						line += std::format(" {}/{} {:02x}{:02x}{:02x}{:02x}", link.area, link.node, link.flags[0], link.flags[1], link.flags[2], link.flags[3]);
					}
					for (const auto& ref : r.refs)
						if (ref.node == k && ref.junction < r.junctions.size())
						{
							const auto& j = r.junctions[ref.junction];
							line += std::format(" | junction {}: x {} y {} z {}..{} {}x{} cells, heights", ref.junction, j.x, j.y, j.minZ, j.maxZ, j.width, j.height);
							for (size_t c = 0; c < std::min<size_t>(static_cast<size_t>(j.width) * j.height, 12); ++c)
								line += std::format(" {}", r.heights[j.heightmap + c]);
						}
					std::printf("%s\n", line.c_str());
				}
			}
		}
		if (g_options.contains("flags"))
		{
			// (how often each value of the links' first two flag bytes comes, from nodes in the box --flags x0,y0,x1,y1)
			float box[4]{};
			std::sscanf(g_options["flags"].c_str(), "%f,%f,%f,%f", &box[0], &box[1], &box[2], &box[3]);
			std::map<std::pair<int, int>, size_t> seen;
			for (const auto& [area, r] : regions)
			{
				if (area >= 1024)
					continue;
				const int cx = static_cast<int>(area % 32), cy = static_cast<int>(area / 32);
				const bool inside = cx >= 8 && cx < 24 && cy >= 8 && cy < 24;
				for (const auto& n : r.nodes)
				{
					const double x = n.X() / 4.0 + (inside ? 0 : cx * 1024 - 16384), y = n.Y() / 4.0 + (inside ? 0 : cy * 1024 - 16384);
					if (x < box[0] || x > box[2] || y < box[1] || y > box[3])
						continue;
					for (uint32_t l = 0; l < n.LinkCount() && n.FirstLink() + l < r.links.size(); ++l)
						++seen[{r.links[n.FirstLink() + l].flags[0], r.links[n.FirstLink() + l].flags[1]}];
				}
			}
			for (const auto& [f, c] : seen)
				std::printf("flags0 %02x flags1 %02x: %zu\n", f.first, f.second, c);
		}
		return bad ? 1 : 0;
	}
	if (command == "ynd-dump")
	{
		// a road-network region file's nodes (--file nodes<N>.ynd): index, place in the world (the region's origin on the
		// loader's 1024 m grid plus the node's own), its links - to see what a region has where
		const fs::path path = g_options["file"];
		std::ifstream in(path, std::ios::binary);
		Bytes data(std::istreambuf_iterator<char>(in), {});
		worldgen::ynd::Region r;
		if (!worldgen::ynd::Read(data, r, error))
			return Fail(error);
		const std::string stem = path.stem().string();
		const int area = std::atoi(stem.c_str() + 5);
		const float ox = (area % 32) * 1024.0f - 16384, oy = (area / 32 % 32) * 1024.0f - 16384;
		std::printf("%s: %zu nodes (%u vehicle), %zu links, %zu junctions; origin (%.0f, %.0f)\n", stem.c_str(), r.nodes.size(), r.vehicleNodes,
		    r.links.size(), r.junctions.size(), ox, oy);
		for (size_t k = 0; k < r.nodes.size(); ++k)
		{
			const auto& n = r.nodes[k];
			std::printf("%zu %s (%.1f, %.1f) links", k, k < r.vehicleNodes ? "v" : "p", ox + n.X() / 4.0f, oy + n.Y() / 4.0f);
			for (uint32_t l = 0; l < n.LinkCount() && n.FirstLink() + l < r.links.size(); ++l)
				std::printf(" %u:%u", r.links[n.FirstLink() + l].area, r.links[n.FirstLink() + l].node);
			std::printf("\n");
		}
		return 0;
	}
	if (command == "extract-ynd")
	{
		// the road-network files the game uses, as they are (to compare against)
		if (!g_options.contains("out"))
			return Fail("--out <dir> is needed");
		fs::create_directories(g_options["out"]);
		size_t n = 0;
		for (uint32_t i = 0; i < 2048; ++i)
			if (const auto* f = files.Find("nodes" + std::to_string(i) + ".ynd"))
			{
				Bytes data;
				if (!files.Read(*f, data, error))
					return Fail(error);
				std::ofstream(fs::path(g_options["out"]) / ("nodes" + std::to_string(i) + ".ynd"), std::ios::binary)
				    .write(reinterpret_cast<const char*>(data.data()), data.size());
				++n;
			}
		std::printf("%zu files\n", n);
		return 0;
	}
	if (command == "ynv-roundtrip")
	{
		// every navmesh the game uses read, written back with our writer (our own page layout) and read again
		size_t count = 0, bad = 0, polys = 0;
		for (const auto& [name, f] : files.Files())
		{
			if (!name.starts_with("navmesh") || !name.ends_with(".ynv"))
				continue;
			Bytes data, written;
			worldgen::ynv::Navmesh m, again;
			std::string why;
			if (!files.Read(f, data, error) || !worldgen::ynv::Read(data, m, error))
				why = "read: " + error;
			else if (!worldgen::ynv::Write(m, written, error) || !worldgen::ynv::Read(written, again, error))
				why = "again: " + error;
			else
				why = worldgen::ynv::Compare(m, again);
			if (!why.empty() && ++bad <= 20)
				std::printf("%s: %s\n", name.c_str(), why.c_str());
			++count;
			polys += m.polys.size();
		}
		std::printf("%zu navmeshes, %zu polygons: %zu differ after writing them again\n", count, polys, bad);
		return bad ? 1 : 0;
	}
	if (command == "testland")
	{
		// a map file placing a straight prop (--piece, its long axis its x) end to end along the polyline at --z: test
		// ground for the far roads (--out <dir>, --name <map name>)
		if (!g_options.contains("out") || !g_options.contains("path"))
			return Fail("--out <dir> --path x,y/x,y[/...] are needed");
		const std::string mapName = g_options.contains("name") ? g_options["name"] : "wb_testland";
		const std::string piece = g_options.contains("piece") ? g_options["piece"] : "stt_prop_track_straight_l";
		const float z = g_options.contains("z") ? std::stof(g_options["z"]) : 100.0f;
		const float length = g_options.contains("length") ? std::stof(g_options["length"]) : 158.95f;
		std::vector<std::array<float, 2>> corners;
		{
			std::istringstream in(g_options["path"]);
			for (std::string item; std::getline(in, item, '/');)
			{
				std::array<float, 2> c{};
				char comma = 0;
				std::istringstream(item) >> c[0] >> comma >> c[1];
				corners.push_back(c);
			}
		}
		struct Piece
		{
			float x, y, heading;
		};
		std::vector<Piece> pieces;
		for (size_t c = 0; c + 1 < corners.size(); ++c)
		{
			const float dx = corners[c + 1][0] - corners[c][0], dy = corners[c + 1][1] - corners[c][1];
			const float span = std::hypot(dx, dy), ux = dx / span, uy = dy / span;
			const float heading = std::atan2(uy, ux) * 180.0f / 3.14159265f; // (the piece's long axis is its x)
			// from 20 m before the corner to 20 m past the next one: the corners overlap
			for (float t = -20.0f; t < span + 20.0f; t += length)
			{
				const float mid = std::min(t + length / 2, span + 20.0f - length / 2);
				pieces.push_back({corners[c][0] + ux * mid, corners[c][1] + uy * mid, heading});
			}
		}
		Bytes templ;
		const auto* tf = files.Find("cs1_10_strm_0.ymap");
		worldgen::meta::Writer w;
		if (!tf || !files.Read(*tf, templ, error) || !w.Template(templ, error))
			return Fail("template map: " + error);
		using worldgen::meta::Hash;
		const uint32_t mapData = Hash("CMapData"), entityDef = Hash("CEntityDef");
		const uint32_t mapSize = w.Size(mapData), entitySize = w.Size(entityDef);
		const auto put = [&](Bytes& b, uint32_t structure, const char* field, const void* value, size_t size) {
			const int at = w.Offset(structure, Hash(field));
			if (at < 0 || at + size > b.size())
				return false;
			std::memcpy(b.data() + at, value, size);
			return true;
		};
		// the entities, 128 to a block, then the block of pointers to them
		std::vector<uint64_t> pointers;
		std::array<float, 3> lo{1e9f, 1e9f, 1e9f}, hi{-1e9f, -1e9f, -1e9f};
		const uint32_t archetype = loader::convert::Joaat(piece);
		const uint32_t perBlock = worldgen::Pages::kSize / entitySize;
		for (size_t i = 0; i < pieces.size(); i += perBlock)
		{
			const size_t n = std::min<size_t>(perBlock, pieces.size() - i);
			Bytes block(n * entitySize, 0);
			const uint32_t number = w.AddBlock(entityDef, {});
			for (size_t k = 0; k < n; ++k)
			{
				const Piece& p = pieces[i + k];
				Bytes e(entitySize, 0);
				const float h = p.heading * 3.14159265f / 180.0f;
				// (a map stores the inverse of an entity's rotation)
				const float position[4] = {p.x, p.y, z, 0}, rotation[4] = {0, 0, -std::sin(h / 2), std::cos(h / 2)};
				const float one = 1.0f, lodDist = 1500.0f, ao = 0;
				const uint32_t guid = loader::convert::Joaat(mapName + std::to_string(i + k)), flags = 0, lodLevel = 5, ao255 = 255;
				const int32_t parent = -1;
				bool ok = put(e, entityDef, "archetypeName", &archetype, 4) && put(e, entityDef, "flags", &flags, 4) && put(e, entityDef, "guid", &guid, 4) &&
				          put(e, entityDef, "position", position, 16) && put(e, entityDef, "rotation", rotation, 16) &&
				          put(e, entityDef, "scaleXY", &one, 4) && put(e, entityDef, "scaleZ", &one, 4) && put(e, entityDef, "parentIndex", &parent, 4) &&
				          put(e, entityDef, "lodDist", &lodDist, 4) && put(e, entityDef, "childLodDist", &ao, 4) &&
				          put(e, entityDef, "lodLevel", &lodLevel, 4) && put(e, entityDef, "ambientOcclusionMultiplier", &ao255, 4) &&
				          put(e, entityDef, "artificialAmbientOcclusion", &ao255, 4);
				if (!ok)
					return Fail("a field of CEntityDef is not in the template");
				std::memcpy(block.data() + k * entitySize, e.data(), entitySize);
				pointers.push_back(worldgen::meta::Writer::Pointer(number, static_cast<uint32_t>(k * entitySize)));
				for (int a = 0; a < 2; ++a)
				{
					lo[a] = std::min(lo[a], (a ? p.y : p.x) - 85.0f);
					hi[a] = std::max(hi[a], (a ? p.y : p.x) + 85.0f);
				}
				lo[2] = std::min(lo[2], z - 5.0f), hi[2] = std::max(hi[2], z + 5.0f);
			}
			w.Block(number) = std::move(block);
		}
		Bytes pointerBlock(pointers.size() * 8);
		std::memcpy(pointerBlock.data(), pointers.data(), pointerBlock.size());
		const uint32_t pointerNumber = w.AddBlock(7, std::move(pointerBlock));
		Bytes root(mapSize, 0);
		const uint32_t name = loader::convert::Joaat(mapName), zero = 0, content = 0x41;
		const float entitiesMin[4] = {lo[0], lo[1], lo[2], 0}, entitiesMax[4] = {hi[0], hi[1], hi[2], 0};
		const float streamMin[4] = {lo[0] - 1500, lo[1] - 1500, lo[2] - 500, 0}, streamMax[4] = {hi[0] + 1500, hi[1] + 1500, hi[2] + 500, 0};
		const uint64_t entities = worldgen::meta::Writer::Pointer(pointerNumber, 0);
		const uint16_t count = static_cast<uint16_t>(pointers.size());
		const int arrayAt = w.Offset(mapData, Hash("entities"));
		if (!put(root, mapData, "name", &name, 4) || !put(root, mapData, "parent", &zero, 4) || !put(root, mapData, "flags", &zero, 4) ||
		    !put(root, mapData, "contentFlags", &content, 4) || !put(root, mapData, "streamingExtentsMin", streamMin, 16) ||
		    !put(root, mapData, "streamingExtentsMax", streamMax, 16) || !put(root, mapData, "entitiesExtentsMin", entitiesMin, 16) ||
		    !put(root, mapData, "entitiesExtentsMax", entitiesMax, 16) || arrayAt < 0)
			return Fail("a field of CMapData is not in the template");
		std::memcpy(root.data() + arrayAt, &entities, 8);
		std::memcpy(root.data() + arrayAt + 8, &count, 2);
		std::memcpy(root.data() + arrayAt + 10, &count, 2);
		const uint32_t rootNumber = w.AddBlock(mapData, std::move(root));
		Bytes file;
		if (!w.Write(rootNumber, file, error))
			return Fail(error);
		const fs::path out = g_options["out"];
		fs::create_directories(out);
		std::ofstream(out / (mapName + ".ymap"), std::ios::binary).write(reinterpret_cast<const char*>(file.data()), file.size());
		std::printf("%s.ymap: %zu pieces of %s\n", mapName.c_str(), pieces.size(), piece.c_str());
		return 0;
	}
	if (command == "navmesh")
	{
		// pedestrians' navmeshes (navgen.hpp) for the entities of a map file (each one's collision, its drawable's,
		// placed as the entity is; every walkable face of the --surface kind) and for collision files (--ybn a,b...: the
		// kind from each face's material)
		if ((!g_options.contains("ymap") && !g_options.contains("ybn")) || !g_options.contains("out"))
			return Fail("--ymap <file> and/or --ybn <file>[,<file>...], and --out <dir> are needed");
		Bytes data;
		if (g_options.contains("ymap"))
		{
			std::ifstream in(g_options["ymap"], std::ios::binary);
			data.assign(std::istreambuf_iterator<char>(in), {});
		}
		ue5export::meta::Meta m;
		if (!data.empty() && !m.Read(data, error))
			return Fail("not a map file: " + error);
		using ue5export::meta::Hash;
		std::map<uint32_t, const ue5export::GameFiles::File*> drawables;
		for (const auto& [name, f] : files.Files())
			if (name.ends_with(".ydr"))
				drawables.emplace(loader::convert::Joaat(name.substr(0, name.size() - 4)), &f);
		const std::string surfaceName = g_options.contains("surface") ? g_options["surface"] : "pavement";
		const worldgen::navgen::Surface surface = surfaceName == "road" ? worldgen::navgen::Surface::Road
		                                          : surfaceName == "ground" ? worldgen::navgen::Surface::Ground
		                                                                    : worldgen::navgen::Surface::Pavement;
		worldgen::bounds::Mesh mesh;
		std::map<uint32_t, std::unique_ptr<loader::convert::Resource>> models;
		size_t placed = 0, missing = 0;
		for (const ue5export::meta::Ref& e : data.empty() ? std::vector<ue5export::meta::Ref>{} : m.Root().Array(Hash("entities")))
		{
			if (!e)
				continue;
			const uint32_t archetype = e.Get<uint32_t>(Hash("archetypeName"));
			auto model = models.find(archetype);
			if (model == models.end())
			{
				std::unique_ptr<loader::convert::Resource> r;
				Bytes file;
				if (const auto d = drawables.find(archetype); d != drawables.end() && files.Read(*d->second, file, error))
				{
					r = std::make_unique<loader::convert::Resource>();
					if (!loader::convert::Read(file, *r, error))
						r.reset();
				}
				model = models.emplace(archetype, std::move(r)).first;
			}
			if (!model->second)
			{
				++missing;
				continue;
			}
			const ue5export::drawable::View view(*model->second);
			const uint64_t bound = view.Get<uint64_t>(ue5export::drawable::View::kRoot + 0xC8);
			if (!bound)
			{
				++missing;
				continue;
			}
			// the entity's transform: a map stores the inverse of its rotation
			const auto p = e.Get<std::array<float, 4>>(Hash("position"));
			const auto s = e.Get<std::array<float, 4>>(Hash("rotation"));
			const float x = -s[0], y = -s[1], z = -s[2], w = s[3], xy = e.Get<float>(Hash("scaleXY")), sz = e.Get<float>(Hash("scaleZ"));
			worldgen::bounds::Matrix t{};
			t[0] = {(1 - 2 * (y * y + z * z)) * xy, 2 * (x * y + z * w) * xy, 2 * (x * z - y * w) * xy};
			t[1] = {2 * (x * y - z * w) * xy, (1 - 2 * (x * x + z * z)) * xy, 2 * (y * z + x * w) * xy};
			t[2] = {2 * (x * z + y * w) * sz, 2 * (y * z - x * w) * sz, (1 - 2 * (x * x + y * y)) * sz};
			t[3] = {p[0], p[1], p[2]};
			if (!worldgen::bounds::Collect(view, bound, t, mesh, error))
				return Fail("collision: " + error);
			++placed;
		}
		std::printf("%zu entities placed (%zu without a drawable or collision): %zu triangles (%zu from boxes, %zu primitives as boxes)\n", placed,
		    missing, mesh.triangles.size(), mesh.counts[1] * 12, mesh.counts[5]);
		if (g_options.contains("obj"))
		{
			std::ofstream obj(g_options["obj"]);
			for (const auto& v : mesh.vertices)
				obj << "v " << v[0] << ' ' << v[1] << ' ' << v[2] << '\n';
			for (const auto& t : mesh.triangles)
				obj << "f " << t[0] + 1 << ' ' << t[1] + 1 << ' ' << t[2] + 1 << '\n';
		}
		std::vector<worldgen::navgen::Surface> surfaces(mesh.triangles.size(), surface);
		if (g_options.contains("ybn"))
		{
			std::istringstream list(g_options["ybn"]);
			for (std::string file; std::getline(list, file, ',');)
			{
				std::ifstream in(file, std::ios::binary);
				Bytes bytes((std::istreambuf_iterator<char>(in)), {});
				loader::convert::Resource res;
				if (!loader::convert::Read(bytes, res, error))
					return Fail(file + ": " + error);
				const size_t first = mesh.triangles.size();
				if (!worldgen::bounds::Collect(ue5export::drawable::View(res), ue5export::drawable::View::kRoot, worldgen::bounds::kIdentity, mesh, error))
					return Fail(file + ": " + error);
				// tarmac roads, concrete and paving pavement, the rest ground
				for (size_t t = first; t < mesh.triangles.size(); ++t)
				{
					const uint8_t type = mesh.materials[t];
					surfaces.push_back(type >= 4 && type <= 6 ? worldgen::navgen::Surface::Road
					                   : type == 1 || type == 15 || type == 174 ? worldgen::navgen::Surface::Pavement
					                                                           : worldgen::navgen::Surface::Ground);
				}
			}
			std::printf("collision files: %zu triangles in all\n", mesh.triangles.size());
		}
		// (--sea z: ground more than half a metre under the sea is for swimming over, not walking)
		if (g_options.contains("sea"))
		{
			const float sea = std::stof(g_options["sea"]) - 0.5f;
			size_t under = 0;
			for (size_t t = 0; t < mesh.triangles.size(); ++t)
				if (std::all_of(mesh.triangles[t].begin(), mesh.triangles[t].end(), [&](uint32_t v) { return mesh.vertices[v][2] < sea; }))
					surfaces[t] = worldgen::navgen::Surface::None, ++under;
			std::printf("%zu triangles under the sea\n", under);
		}
		worldgen::navgen::Result result;
		if (!worldgen::navgen::Build(mesh, surfaces, {}, result, error))
			return Fail(error);
		const fs::path out = g_options["out"];
		fs::create_directories(out);
		for (const auto& [square, nm] : result.navmeshes)
		{
			Bytes file;
			if (!worldgen::ynv::Write(nm, file, error))
				return Fail(worldgen::navgen::FileName(square) + ": " + error);
			std::ofstream(out / worldgen::navgen::FileName(square), std::ios::binary).write(reinterpret_cast<const char*>(file.data()), file.size());
			std::printf("%s: index %u, %zu polygons, %zu vertices\n", worldgen::navgen::FileName(square).c_str(), nm.index, nm.polys.size(), nm.vertices.size());
		}
		std::printf("%zu navmeshes, %zu polygons; %zu edges joined across squares, %zu vertices put into edges on their sides\n", result.navmeshes.size(),
		    result.polygons, result.joined, result.cut);
		return 0;
	}
	if (command == "testground")
	{
		// a collision file of flat ground (no model): --at x,y (its south-west corner) --size m --z height --cell m
		// --material type --name <file name> --out <dir>
		if (!g_options.contains("at") || !g_options.contains("out") || !g_options.contains("name"))
			return Fail("--at x,y --out <dir> --name <name> are needed");
		float x0 = 0, y0 = 0;
		char comma = 0;
		std::istringstream(g_options["at"]) >> x0 >> comma >> y0;
		const float size = g_options.contains("size") ? std::stof(g_options["size"]) : 150.0f;
		const float z = g_options.contains("z") ? std::stof(g_options["z"]) : 100.0f;
		const float cell = g_options.contains("cell") ? std::stof(g_options["cell"]) : 5.0f;
		const uint8_t type = static_cast<uint8_t>(g_options.contains("material") ? std::stoi(g_options["material"]) : 47);
		const int cells = std::max(1, static_cast<int>(std::lround(size / cell)));
		// parts of at most 20 x 20 cells (800 triangles)
		std::vector<worldgen::ybn::Part> parts;
		for (int py = 0; py < cells; py += 20)
			for (int px = 0; px < cells; px += 20)
			{
				worldgen::ybn::Part part;
				part.materials.push_back(worldgen::ybn::Material(type));
				const int nx = std::min(20, cells - px), ny = std::min(20, cells - py);
				for (int j = 0; j <= ny; ++j)
					for (int i = 0; i <= nx; ++i)
						part.vertices.push_back({x0 + (px + i) * cell, y0 + (py + j) * cell, z});
				for (int j = 0; j < ny; ++j)
					for (int i = 0; i < nx; ++i)
					{
						const uint32_t a = j * (nx + 1) + i, b = a + 1, c = a + nx + 1, d = c + 1;
						part.triangles.push_back({a, b, d});
						part.triangles.push_back({a, d, c});
						part.triangleMaterials.push_back(0);
						part.triangleMaterials.push_back(0);
					}
				parts.push_back(std::move(part));
			}
		Bytes file;
		if (!worldgen::ybn::Write(parts, file, error))
			return Fail(error);
		const fs::path out = g_options["out"];
		fs::create_directories(out);
		std::ofstream(out / (g_options["name"] + ".ybn"), std::ios::binary).write(reinterpret_cast<const char*>(file.data()), file.size());
		// read back: every triangle where it was put (within the quantum)
		loader::convert::Resource res;
		worldgen::bounds::Mesh back;
		if (!loader::convert::Read(file, res, error) || !worldgen::bounds::Collect(ue5export::drawable::View(res), ue5export::drawable::View::kRoot,
		                                                worldgen::bounds::kIdentity, back, error))
			return Fail("reading it back: " + error);
		size_t triangles = 0;
		for (const auto& p : parts)
			triangles += p.triangles.size();
		float worst = 0;
		for (const auto& v : back.vertices)
			worst = std::max(worst, std::fabs(v[2] - z));
		std::printf("%s.ybn: %zu parts, %zu triangles (read back %zu, heights within %.4f m), %zu bytes\n", g_options["name"].c_str(), parts.size(),
		    triangles, back.triangles.size(), worst, file.size());
		return 0;
	}
	if (command == "texture-find")
	{
		// texture dictionaries' textures whose names contain any of --pattern a,b,c (case-insensitive): dictionary, name,
		// size, format
		std::vector<std::string> patterns;
		std::istringstream list(g_options["pattern"]);
		for (std::string p; std::getline(list, p, ',');)
		{
			std::transform(p.begin(), p.end(), p.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
			patterns.push_back(p);
		}
		size_t read = 0;
		for (const auto& [name, f] : files.Files())
		{
			if (!name.ends_with(".ytd"))
				continue;
			Bytes data;
			loader::convert::Resource res;
			if (!files.Read(f, data, error) || !loader::convert::Read(data, res, error))
				continue;
			++read;
			const ue5export::drawable::View view(res);
			for (const auto& t : ue5export::drawable::Textures(view, ue5export::drawable::View::kRoot))
			{
				std::string lower = t.name;
				std::transform(lower.begin(), lower.end(), lower.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
				if (std::any_of(patterns.begin(), patterns.end(), [&](const std::string& p) { return lower.find(p) != std::string::npos; }))
					std::printf("%s %s %ux%u format %u\n", name.c_str(), t.name.c_str(), t.width, t.height, t.dxgi);
			}
		}
		std::fprintf(stderr, "%zu dictionaries read\n", read);
		return 0;
	}
	if (command == "flora-test")
	{
		// our own trees (flora.hpp): their pictures as PNGs and each kind's models' sizes and triangles: --out <dir>
		const fs::path out = g_options["out"];
		fs::create_directories(out);
		for (int t = 0; t < worldgen::flora::kTextures; ++t)
			if (!worldgen::texture::WritePng(out / std::format("flora_{}.png", t), worldgen::flora::Picture(static_cast<worldgen::flora::Texture>(t)), error))
				return Fail(error);
		for (int s = 0; s < worldgen::flora::kSpecies; ++s)
			for (int k = 0; k < 4; ++k)
			{
				const auto m = worldgen::flora::Make(static_cast<worldgen::flora::Species>(s), static_cast<uint32_t>(s * 101 + k * 7 + 1));
				// (--obj 1: the nearest level as an OBJ, a group per picture, texture coordinates kept)
				if (g_options.contains("obj"))
				{
					std::ofstream obj(out / std::format("tree_{}_{}.obj", s, k));
					size_t base = 1;
					for (int t = 0; t < worldgen::flora::kTextures; ++t)
					{
						const auto& mesh = m.lods[0][t];
						if (mesh.indices.empty())
							continue;
						obj << "g picture" << t << "\n";
						for (const auto& v : mesh.vertices)
							obj << std::format("v {} {} {}\nvt {} {}\n", v.position[0], v.position[1], v.position[2], v.uv[0][0], v.uv[0][1]);
						for (size_t i = 0; i + 2 < mesh.indices.size(); i += 3)
							obj << std::format("f {0}/{0} {1}/{1} {2}/{2}\n", base + mesh.indices[i], base + mesh.indices[i + 1], base + mesh.indices[i + 2]);
						base += mesh.vertices.size();
					}
				}
				std::printf("kind %d model %d: %.1f m tall, %.1f m wide, trunk %.2f m; triangles by level:", s, k, m.height, m.width, m.trunk);
				for (const auto& lod : m.lods)
				{
					size_t n = 0;
					for (const auto& mesh : lod)
						n += mesh.indices.size() / 3;
					std::printf(" %zu", n);
				}
				std::printf("\n");
			}
		return 0;
	}
	if (command == "texture-png")
	{
		// one of a texture dictionary's textures as a PNG (its first mip, decoded): --dictionary <file.ytd> --texture <name>
		// --out <file.png>
		Bytes data;
		loader::convert::Resource res;
		const auto* f = files.Find(g_options["dictionary"]);
		if (!f || !files.Read(*f, data, error) || !loader::convert::Read(data, res, error))
			return Fail("cannot read " + g_options["dictionary"] + ": " + error);
		std::string wanted = g_options["texture"];
		std::transform(wanted.begin(), wanted.end(), wanted.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
		for (const auto& t : ue5export::drawable::Textures(ue5export::drawable::View(res), ue5export::drawable::View::kRoot))
		{
			std::string lower = t.name;
			std::transform(lower.begin(), lower.end(), lower.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
			if (lower != wanted)
				continue;
			const bool compact = t.dxgi == 71 || t.dxgi == 72 || t.dxgi == 80 || t.dxgi == 81; // (8-byte blocks)
			loader::convert::TextureFormat format{t.dxgi, compact ? 8u : 16u, true};
			uint8_t mips = t.mips;
			Bytes pixels(t.data.begin(), t.data.end());
			loader::convert::MakeRenderTargetFormat(format, mips, static_cast<uint16_t>(t.width), static_cast<uint16_t>(t.height), pixels);
			worldgen::texture::Image image(t.width, t.height);
			for (size_t i = 0; i < image.pixels.size() && 4 * i + 3 < pixels.size(); ++i)
				image.pixels[i] = {pixels[4 * i + 2], pixels[4 * i + 1], pixels[4 * i], pixels[4 * i + 3]};
			if (!worldgen::texture::WritePng(g_options["out"], image, error))
				return Fail(error);
			std::printf("%s: %ux%u format %u -> %s\n", t.name.c_str(), t.width, t.height, t.dxgi, g_options["out"].c_str());
			return 0;
		}
		return Fail(g_options["texture"] + " is not in " + g_options["dictionary"]);
	}
	if (command == "shader-find")
	{
		// the game's models (.ydr) with a shader of these effects: --shader name,name (file, the shader's index, its
		// textures)
		std::set<uint32_t> wanted;
		std::istringstream list(g_options["shader"]);
		for (std::string s; std::getline(list, s, ',');)
			wanted.insert(loader::convert::Joaat(s));
		for (const auto& [name, f] : files.Files())
		{
			if (!name.ends_with(".ydr"))
				continue;
			Bytes data;
			loader::convert::Resource res;
			if (!files.Read(f, data, error) || !loader::convert::Read(data, res, error))
				continue;
			ue5export::drawable::Model model;
			if (!ue5export::drawable::Read(ue5export::drawable::View(res), ue5export::drawable::View::kRoot, model, error))
				continue;
			for (size_t k = 0; k < model.materials.size(); ++k)
				if (wanted.contains(model.materials[k].effect))
				{
					std::printf("%s %zu %08x:", name.c_str(), k, model.materials[k].effect);
					for (const auto& [p, texture] : model.materials[k].textures)
						std::printf(" %s", texture.c_str());
					std::printf("\n");
				}
		}
		return 0;
	}
	if (command == "hash-find")
	{
		// the game's files whose name (without the extension) has one of these hashes: --hash 0x...,0x...
		std::set<uint32_t> wanted;
		std::istringstream list(g_options["hash"]);
		for (std::string h; std::getline(list, h, ',');)
			wanted.insert(static_cast<uint32_t>(std::stoul(h, nullptr, 16)));
		for (const auto& [name, f] : files.Files())
		{
			const auto slash = name.find_last_of('/');
			std::string stem = name.substr(slash == std::string::npos ? 0 : slash + 1);
			if (const auto dot = stem.find('.'); dot != std::string::npos)
				stem.resize(dot);
			if (wanted.contains(loader::convert::Joaat(stem)))
				std::printf("%08x %s\n", loader::convert::Joaat(stem), name.c_str());
		}
		return 0;
	}
	if (command == "ydd-stats")
	{
		// a drawable dictionary on disk: each drawable's name hash, box, shaders (effect, textures), sections (vertices,
		// x/y/z ranges, the first vertex colour's range): --file <file.ydd>
		std::ifstream in(g_options["file"], std::ios::binary);
		Bytes data((std::istreambuf_iterator<char>(in)), {});
		loader::convert::Resource res;
		if (data.empty() || !loader::convert::Read(data, res, error))
			return Fail("cannot read " + g_options["file"] + ": " + error);
		const ue5export::drawable::View view(res);
		for (const auto& [hash, at] : ue5export::drawable::Drawables(view, "ydd"))
		{
			ue5export::drawable::Model model;
			if (!ue5export::drawable::Read(view, at, model, error))
			{
				std::printf("drawable %08x: %s\n", hash, error.c_str());
				continue;
			}
			std::printf("drawable %08x: box %.1f %.1f %.1f .. %.1f %.1f %.1f, %zu shaders, %zu sections\n", hash, model.bbMin[0], model.bbMin[1], model.bbMin[2],
			    model.bbMax[0], model.bbMax[1], model.bbMax[2], model.materials.size(), model.sections.size());
			for (const auto& m : model.materials)
			{
				std::printf("  shader %08x bucket %u:", m.effect, m.drawBucket);
				for (const auto& [p, name] : m.textures)
					std::printf(" %08x=%s", p, name.c_str());
				for (const auto& [p, values] : m.constants)
				{
					std::printf(" %08x=(", p);
					for (size_t k = 0; k < values.size() && k < 4; ++k)
						std::printf("%s%g", k ? " " : "", values[k]);
					std::printf(")");
				}
				std::printf("\n");
			}
			for (const auto& s : model.sections)
			{
				float lo[3] = {1e9f, 1e9f, 1e9f}, hi[3] = {-1e9f, -1e9f, -1e9f};
				for (size_t v = 0; v < s.vertices; ++v)
					for (int c = 0; c < 3; ++c)
						lo[c] = std::min(lo[c], s.position[v * 3 + c]), hi[c] = std::max(hi[c], s.position[v * 3 + c]);
				std::printf("  section: shader %u, %u vertices, %zu triangles, x %.1f..%.1f y %.1f..%.1f z %.2f..%.2f", s.material, s.vertices, s.indices.size() / 3,
				    lo[0], hi[0], lo[1], hi[1], lo[2], hi[2]);
				if (s.color0.size() >= static_cast<size_t>(s.vertices) * 4 && s.vertices)
					std::printf(", colour0 first (%u %u %u %u)", s.color0[0], s.color0[1], s.color0[2], s.color0[3]);
				std::printf("\n");
				// (--triangles <file>: every triangle as a line "x y x y x y r g b z", appended)
				if (g_options.contains("triangles") && s.color0.size() >= static_cast<size_t>(s.vertices) * 4)
				{
					std::ofstream list(g_options["triangles"], std::ios::app);
					for (size_t t = 0; t + 2 < s.indices.size(); t += 3)
					{
						const uint32_t a = s.indices[t];
						for (int k = 0; k < 3; ++k)
							list << s.position[s.indices[t + k] * 3] << ' ' << s.position[s.indices[t + k] * 3 + 1] << ' ';
						list << int(s.color0[a * 4]) << ' ' << int(s.color0[a * 4 + 1]) << ' ' << int(s.color0[a * 4 + 2]) << ' ' << s.position[a * 3 + 2] << '\n';
					}
				}
				// (--colours: the triangles by their first vertex's colour and height, with the area they cover)
				if (g_options.contains("colours") && s.color0.size() >= static_cast<size_t>(s.vertices) * 4)
				{
					std::map<std::pair<uint32_t, float>, std::pair<int, double>> kinds;
					for (size_t t = 0; t + 2 < s.indices.size(); t += 3)
					{
						const uint32_t a = s.indices[t], b = s.indices[t + 1], c = s.indices[t + 2];
						uint32_t colour;
						std::memcpy(&colour, &s.color0[a * 4], 4);
						const float* p = s.position.data();
						const double area = std::fabs((p[b * 3] - p[a * 3]) * (p[c * 3 + 1] - p[a * 3 + 1]) - (p[c * 3] - p[a * 3]) * (p[b * 3 + 1] - p[a * 3 + 1])) / 2;
						auto& k = kinds[{colour, p[a * 3 + 2]}];
						++k.first, k.second += area;
					}
					for (const auto& [key, value] : kinds)
						std::printf("    colour %u %u %u %u z %.1f: %d triangles, %.0f m2\n", key.first & 0xFF, (key.first >> 8) & 0xFF, (key.first >> 16) & 0xFF, key.first >> 24,
						    key.second, value.first, value.second);
				}
			}
		}
		return 0;
	}
	if (command == "ydr-stats")
	{
		// a model file on disk (ours, for checking): each section's vertices, heights and vertex colours: --file <file.ydr>
		std::ifstream in(g_options["file"], std::ios::binary);
		Bytes data((std::istreambuf_iterator<char>(in)), {});
		loader::convert::Resource res;
		if (data.empty() || !loader::convert::Read(data, res, error))
			return Fail("cannot read " + g_options["file"] + ": " + error);
		const ue5export::drawable::View view(res);
		ue5export::drawable::Model model;
		if (!ue5export::drawable::Read(view, ue5export::drawable::View::kRoot, model, error))
			return Fail(error);
		for (const auto& m : model.materials)
			std::printf("material effect %08x, %zu textures, bucket %u\n", m.effect, m.textures.size(), m.drawBucket);
		for (const auto& s : model.sections)
		{
			float zMin = 1e9f, zMax = -1e9f;
			for (size_t v = 0; v < s.vertices; ++v)
				zMin = std::min(zMin, s.position[v * 3 + 2]), zMax = std::max(zMax, s.position[v * 3 + 2]);
			std::printf("section: material %u, %u vertices, %zu triangles, z %.2f..%.2f\n", s.material, s.vertices, s.indices.size() / 3, zMin, zMax);
			// (each texture coordinate set: its range, and how many units a metre along x and y - least squares)
			for (size_t set = 0; set < s.uv.size(); ++set)
			{
				const auto& uv = s.uv[set];
				if (uv.size() < static_cast<size_t>(s.vertices) * 2 || !s.vertices)
					continue;
				float lo[2] = {1e9f, 1e9f}, hi[2] = {-1e9f, -1e9f};
				double sxx = 0, sx = 0, su = 0, sxu = 0, syy = 0, sy = 0, sv = 0, syv = 0;
				for (size_t v = 0; v < s.vertices; ++v)
				{
					for (int c = 0; c < 2; ++c)
						lo[c] = std::min(lo[c], uv[v * 2 + c]), hi[c] = std::max(hi[c], uv[v * 2 + c]);
					const double x = s.position[v * 3], y = s.position[v * 3 + 1], u = uv[v * 2], w = uv[v * 2 + 1];
					sxx += x * x, sx += x, su += u, sxu += x * u, syy += y * y, sy += y, sv += w, syv += y * w;
				}
				const double n = s.vertices;
				const double du = (n * sxu - sx * su) / std::max(1e-9, n * sxx - sx * sx), dv = (n * syv - sy * sv) / std::max(1e-9, n * syy - sy * sy);
				std::printf("  uv%zu: u %.3f..%.3f, v %.3f..%.3f; per metre: u/x %.4f, v/y %.4f\n", set, lo[0], hi[0], lo[1], hi[1], du, dv);
			}
			for (const auto* colours : {&s.color0, &s.color1})
			{
				if (colours->size() < static_cast<size_t>(s.vertices) * 4)
					continue;
				for (int c = 0; c < 4; ++c)
				{
					int lo = 255, hi = 0;
					double sum = 0;
					for (size_t v = 0; v < s.vertices; ++v)
					{
						const int x = (*colours)[v * 4 + c];
						lo = std::min(lo, x), hi = std::max(hi, x), sum += x;
					}
					std::printf("  colour%d %c: %d..%d, mean %.1f\n", colours == &s.color0 ? 0 : 1, "RGBA"[c], lo, hi, sum / s.vertices);
				}
			}
		}
		return 0;
	}
	if (command == "ytyp-check")
	{
		// one of our archetype files on disk (--file): its archetypes, the names twice, the boxes not finite or huge, the
		// lodDists out of range - what makes the game pass the whole file over
		std::ifstream in(g_options["file"], std::ios::binary);
		Bytes data((std::istreambuf_iterator<char>(in)), {});
		ue5export::meta::Meta m;
		if (data.empty() || !m.Read(data, error))
			return Fail("cannot read " + g_options["file"] + ": " + error);
		using ue5export::meta::Hash;
		std::map<uint32_t, int> seen;
		size_t count = 0, odd = 0;
		for (const ue5export::meta::Ref& a : m.Root().Array(Hash("archetypes")))
		{
			if (!a)
				continue;
			++count;
			const uint32_t nameHash = a.Get<uint32_t>(Hash("name"));
			++seen[nameHash];
			const auto lo = a.Get<std::array<float, 4>>(Hash("bbMin")), hi = a.Get<std::array<float, 4>>(Hash("bbMax"));
			const auto centre = a.Get<std::array<float, 4>>(Hash("bsCentre"));
			const float radius = a.Get<float>(Hash("bsRadius")), lod = a.Get<float>(Hash("lodDist"));
			bool bad = !(lod > 0 && lod < 20000) || !(radius >= 0 && radius < 20000);
			for (int k = 0; k < 3; ++k)
				bad |= !std::isfinite(lo[k]) || !std::isfinite(hi[k]) || !std::isfinite(centre[k]) || std::fabs(lo[k]) > 20000 || std::fabs(hi[k]) > 20000 ||
				       lo[k] > hi[k];
			if (bad && odd++ < 20)
				std::printf("odd archetype %08x: box %g %g %g .. %g %g %g, sphere %g %g %g r %g, lodDist %g\n", nameHash, lo[0], lo[1], lo[2], hi[0], hi[1], hi[2],
				    centre[0], centre[1], centre[2], radius, lod);
		}
		size_t twice = 0;
		for (const auto& [h, n] : seen)
			twice += n > 1;
		std::printf("%zu archetypes, %zu odd, %zu names more than once\n", count, odd, twice);
		return 0;
	}
	if (command == "vertex-dump")
	{
		// a game model's vertices as they are (position, normal, colour, first texture coordinates) and triangles, for
		// working out what a shader reads from them: --model <file.ydr> [--count 40]; or a build's own on the disk: --file
		// <path.ydr>
		Bytes data;
		loader::convert::Resource res;
		if (g_options.contains("file"))
		{
			std::ifstream in(g_options["file"], std::ios::binary);
			data.assign(std::istreambuf_iterator<char>(in), {});
			if (data.empty() || !loader::convert::Read(data, res, error))
				return Fail("cannot read " + g_options["file"] + ": " + error);
		}
		else if (const auto* f = files.Find(g_options["model"]); !f || !files.Read(*f, data, error) || !loader::convert::Read(data, res, error))
			return Fail("cannot read " + g_options["model"] + ": " + error);
		ue5export::drawable::Model model;
		if (!ue5export::drawable::Read(ue5export::drawable::View(res), ue5export::drawable::View::kRoot, model, error))
			return Fail(error);
		const size_t count = g_options.count("count") ? std::stoul(g_options["count"]) : 40;
		for (const auto& s : model.sections)
		{
			std::printf("section: material %u, lod %u, %u vertices, %zu triangles\n", s.material, s.lod, s.vertices, s.indices.size() / 3);
			for (size_t v = 0; v < std::min<size_t>(s.vertices, count); ++v)
			{
				std::printf("  %3zu p %8.3f %8.3f %8.3f", v, s.position[v * 3], s.position[v * 3 + 1], s.position[v * 3 + 2]);
				if (s.normal.size() >= (v + 1) * 3)
					std::printf(" n %7.3f %7.3f %7.3f", s.normal[v * 3], s.normal[v * 3 + 1], s.normal[v * 3 + 2]);
				if (s.color0.size() >= (v + 1) * 4)
					std::printf(" c %3u %3u %3u %3u", s.color0[v * 4], s.color0[v * 4 + 1], s.color0[v * 4 + 2], s.color0[v * 4 + 3]);
				if (s.color1.size() >= (v + 1) * 4)
					std::printf(" c1 %3u %3u %3u %3u", s.color1[v * 4], s.color1[v * 4 + 1], s.color1[v * 4 + 2], s.color1[v * 4 + 3]);
				for (const auto& uv : s.uv)
					if (uv.size() >= (v + 1) * 2)
						std::printf(" uv %7.3f %7.3f", uv[v * 2], uv[v * 2 + 1]);
				std::printf("\n");
			}
			std::printf("  triangles:");
			for (size_t i = 0; i < std::min<size_t>(s.indices.size(), count * 3); ++i)
				std::printf("%s%u", i % 3 ? "," : " ", s.indices[i]);
			std::printf("\n");
		}
		return 0;
	}
	if (command == "material-dump")
	{
		// a game model's shader as worldgen reads it for its own models: --model <file.ydr> --index <n>
		Bytes data;
		loader::convert::Resource res;
		const auto* f = files.Find(g_options["model"]);
		if (!f || !files.Read(*f, data, error) || !loader::convert::Read(data, res, error))
			return Fail("cannot read " + g_options["model"] + ": " + error);
		worldgen::ydr::Material m;
		if (!worldgen::ydr::ReadMaterial(ue5export::drawable::View(res), ue5export::drawable::View::kRoot, std::stoul(g_options["index"]), m, error))
			return Fail(error);
		std::printf("effect %08x bucket %u block %u\n", m.effect, m.bucket, m.block);
		for (const auto& [p, n] : m.textures)
			std::printf("  texture %08x %s\n", p, n.c_str());
		for (const auto& [p, bytes] : m.constants)
		{
			std::printf("  constant %08x (%zu bytes):", p, bytes.size());
			for (size_t k = 0; k + 3 < bytes.size() && k < 64; k += 4)
			{
				float v;
				std::memcpy(&v, bytes.data() + k, 4);
				std::printf(" %g", v);
			}
			std::printf("\n");
		}
		std::printf("  slots (DXGI formats):");
		for (size_t k = 0; k < m.slots.size(); ++k)
			std::printf(" %u (%u)", m.slots[k], k < m.formats.size() ? m.formats[k] : 0u);
		std::printf("\n");
		return 0;
	}
	if (command == "tint-stats")
	{
		// how the game's own maps tint these archetypes (entity tintValue: the row of a tree's palette): --names a,b,c
		std::map<uint32_t, std::string> wanted;
		std::istringstream list(g_options["names"]);
		for (std::string n; std::getline(list, n, ',');)
			wanted[loader::convert::Joaat(n)] = n;
		std::map<std::string, std::map<uint32_t, int>> counts;
		for (const auto& [name, f] : files.Files())
		{
			if (!name.ends_with(".ymap"))
				continue;
			Bytes data;
			ue5export::meta::Meta m;
			if (!files.Read(f, data, error) || !m.Read(data, error))
				continue;
			using ue5export::meta::Hash;
			for (const ue5export::meta::Ref& e : m.Root().Array(Hash("entities")))
				if (e)
					if (const auto it = wanted.find(e.Get<uint32_t>(Hash("archetypeName"))); it != wanted.end())
						++counts[it->second][e.Get<uint32_t>(Hash("tintValue"))];
		}
		for (const auto& [name, tints] : counts)
		{
			std::printf("%s:", name.c_str());
			for (const auto& [tint, n] : tints)
				std::printf(" %u x%d", tint, n);
			std::printf("\n");
		}
		return 0;
	}
	if (command == "ls-far")
	{
		// Los Santos's far views: its maps of long-range and LOD models (the game's own: <area>_..._long_<n>, <area>_lod<n>),
		// which the loader puts away while the player is on a new land (src/loader/game/sectors.cpp; their mountains stood
		// over Florida's horizon): --out <file> (ModLoader\data\ls_far_maps.txt), a name a line
		std::set<std::string> names;
		for (const auto& [name, f] : files.Files())
		{
			if (!name.ends_with(".ymap") || name.starts_with("wb"))
				continue;
			const std::string stem = name.substr(0, name.size() - 5);
			const size_t lod = stem.rfind("_lod"), longRange = stem.rfind("_long_");
			const auto digits = [&](size_t from) { return std::all_of(stem.begin() + static_cast<ptrdiff_t>(from), stem.end(), [](char c) { return c >= '0' && c <= '9'; }); };
			if ((lod != std::string::npos && stem.size() - lod <= 5 && digits(lod + 4)) || (longRange != std::string::npos && longRange + 6 < stem.size() && digits(longRange + 6)))
				names.insert(stem);
		}
		std::ofstream out(g_options["out"], std::ios::binary);
		for (const auto& n : names)
			out << n << "\n";
		std::printf("%zu maps\n", names.size());
		return out ? 0 : 1;
	}
	if (command == "file-find")
	{
		// the game's files whose names contain --pattern, with where they are; --out <dir>: their contents too
		const std::string pattern = g_options["pattern"];
		for (const auto& [name, f] : files.Files())
		{
			if (name.find(pattern) == std::string::npos)
				continue;
			std::printf("%s  %s\n", name.c_str(), f.path.c_str());
			Bytes data;
			if (g_options.contains("out") && files.Read(f, data, error))
			{
				fs::create_directories(g_options["out"]);
				std::ofstream(fs::path(g_options["out"]) / name, std::ios::binary).write(reinterpret_cast<const char*>(data.data()), data.size());
			}
		}
		return 0;
	}
	if (command == "rpf-find")
	{
		// the paths inside an archive (and the archives in it) that contain --pattern; with --out also their contents
		if (!g_options.contains("rpf") || !g_options.contains("pattern"))
			return Fail("--rpf <file> --pattern <text> are needed");
		const fs::path path = g_options["rpf"];
		auto in = std::make_shared<std::ifstream>(path, std::ios::binary);
		loader::convert::Archive root;
		if (!in->is_open() || !root.Open(in, 0, fs::file_size(path), path.filename().string(), &decrypt, error))
			return Fail("cannot open " + path.string() + ": " + error);
		const std::string pattern = g_options["pattern"];
		const fs::path out = g_options.contains("out") ? fs::path(g_options["out"]) : fs::path();
		const auto walk = [&](auto&& self, const loader::convert::Archive& a, uint32_t node, const std::string& prefix) -> void {
			for (const uint32_t c : a.Nodes()[node].children)
			{
				const auto& n = a.Nodes()[c];
				const std::string p = prefix + "/" + n.name;
				if (n.directory)
					self(self, a, c, p);
				else if (n.name.ends_with(".rpf"))
				{
					loader::convert::Archive nested;
					if (a.OpenNested(c, nested, error))
						self(self, nested, 0, p);
				}
				if (!n.directory && p.find(pattern) != std::string::npos)
				{
					std::printf("%s (%u bytes)\n", p.c_str(), n.size);
					Bytes data;
					if (!out.empty() && a.ReadFile(c, data, error))
					{
						fs::create_directories(out);
						std::ofstream(out / n.name, std::ios::binary).write(reinterpret_cast<const char*>(data.data()), data.size());
					}
				}
			}
		};
		walk(walk, root, 0, "");
		return 0;
	}
	if (command == "pack")
	{
		// DLC packs of the files in --in (maps, navmeshes, models...): ModLoader\mods\<name>\dlc.rpf, and when they come
		// to more than one archive holds the rest in packs of their own beside it, <name>_2, <name>_3... (in the order the
		// files come; the game's stores are shared: a map in one pack finds its models, archetypes and parent map in another).
		if (!g_options.contains("in") || !g_options.contains("out") || !g_options.contains("name"))
			return Fail("--in <dir> --out <dlc.rpf> --name <pack name> are needed");
		const std::string name = g_options["name"];
		const fs::path out = g_options["out"];
		std::vector<loader::convert::WriteNode> nodes;
		std::string parentTxds;
		for (const auto& entry : fs::directory_iterator(g_options["in"]))
		{
			if (!entry.is_regular_file())
				continue;
			std::string file = entry.path().filename().string();
			std::transform(file.begin(), file.end(), file.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
			if (file == "land.json" || file == "water.bin" || file == "parked.bin")
			{
				// (the loader's, next to the pack: game/sea.cpp, game/minimap.cpp, game/sectors.cpp's parked cars)
				std::error_code ec;
				fs::create_directories(out.parent_path(), ec);
				fs::copy_file(entry.path(), out.parent_path() / file, fs::copy_options::overwrite_existing, ec);
				continue;
			}
			if (file.ends_with(".txt") || file.ends_with(".png") || file.ends_with(".json"))
				continue; // (the build's notes, not game files)
			if (file == "gtxd.meta")
			{
				// (texture dictionaries' parents: the pack's data, not streamed)
				std::ifstream in(entry.path(), std::ios::binary);
				parentTxds.assign(std::istreambuf_iterator<char>(in), {});
				continue;
			}
			loader::convert::WriteNode n;
			n.name = file;
			std::ifstream in(entry.path(), std::ios::binary);
			n.data.assign(std::istreambuf_iterator<char>(in), {});
			n.kind = n.data.size() >= 4 && std::memcmp(n.data.data(), "RSC7", 4) == 0 ? loader::convert::WriteNode::Kind::Resource
			                                                                          : loader::convert::WriteNode::Kind::File;
			nodes.push_back(std::move(n));
		}
		// (3.5 GB a pack, the rest of the 4 GB its archive can hold for its table and its inner archives' own)
		constexpr size_t kPackBytes = size_t{3500} << 20;
		std::vector<std::vector<loader::convert::WriteNode>> packs(1);
		size_t bytes = 0;
		for (auto& n : nodes)
		{
			const size_t size = (n.data.size() + 511) / 512 * 512;
			if (!packs.back().empty() && bytes + size > kPackBytes)
			{
				packs.emplace_back();
				bytes = 0;
			}
			bytes += size;
			packs.back().push_back(std::move(n));
		}
		// (packs an earlier build needed past these: written empty - the game holds an enabled pack's archive open)
		const fs::path mods = out.parent_path().parent_path();
		const auto packName = [&](size_t p) { return p ? std::format("{}_{}", name, p + 1) : name; };
		for (size_t p = packs.size(); fs::is_directory(mods / packName(p)); ++p)
			packs.emplace_back();
		for (size_t p = 0; p < packs.size(); ++p)
			// (the overlay - the start-up change set's - also has all the navmeshes: a land that shares its place with
			// others (game/sectors.hpp) puts its maps away first, the game's session restart lets its navmeshes go, then its
			// overlay; a map archive's navmesh still loaded when it is taken away stops the game)
			if (!WritePack(packName(p), std::move(packs[p]), p ? std::string() : parentTxds, p ? fs::path() : fs::path(g_options["in"]) / "requests.txt",
			        [&](const std::string& file) { return files.Find(file) != nullptr || file.ends_with(".ynv"); }, p ? mods / packName(p) / out.filename() : out,
			        error))
				return Fail(packName(p) + ": " + error);
		return 0;
	}
	if (command == "testterrain")
	{
		// a small island of generated ground (models, collision, archetypes, map) for testing: --at x,y (its south-west
		// corner) --size m --cell m --tile m --name <n> --out <dir>. A hill in the middle, grass, then a sand beach
		// into the sea; textures of the game's beach (Vespucci), the shader of its airport ground.
		if (!g_options.contains("at") || !g_options.contains("out"))
			return Fail("--at x,y --out <dir> are needed");
		float x0 = 0, y0 = 0;
		char comma = 0;
		std::istringstream(g_options["at"]) >> x0 >> comma >> y0;
		const std::string name = g_options.contains("name") ? g_options["name"] : "wb_terrain";
		const float size = g_options.contains("size") ? std::stof(g_options["size"]) : 400.0f;
		const float cell = g_options.contains("cell") ? std::stof(g_options["cell"]) : 2.0f;
		const float tile = g_options.contains("tile") ? std::stof(g_options["tile"]) : 100.0f;
		worldgen::terrain::Field f;
		f.x0 = x0, f.y0 = y0, f.cell = cell;
		f.nx = f.ny = static_cast<int>(std::lround(size / cell));
		const float cx = x0 + size / 2, cy = y0 + size / 2;
		for (int j = 0; j <= f.ny; ++j)
			for (int i = 0; i <= f.nx; ++i)
			{
				const float x = x0 + i * cell, y = y0 + j * cell;
				// distance from the middle (0..1 at the edge's middle), a little wavy
				const float r = std::hypot(x - cx, y - cy) / (size / 2) + 0.04f * std::sin(x * 0.031f) * std::cos(y * 0.027f);
				float h;
				if (r < 0.55f)
					h = 3.0f + 9.0f * (1 - r / 0.55f) * (1 - r / 0.55f) + 0.6f * std::sin(x * 0.07f) * std::sin(y * 0.05f);
				else if (r < 0.75f)
					h = 3.0f - 2.6f * (r - 0.55f) / 0.2f; // down to the beach
				else
					h = 0.4f - 9.0f * (r - 0.75f) / 0.3f; // the beach into the sea
				f.height.push_back(h);
				// grass on top, soft sand below 2.4 m, wet sand below 0.6 m, hard sand where it is steep
				const float grass = std::clamp((h - 2.0f) / 0.8f, 0.0f, 1.0f), wet = std::clamp((0.9f - h) / 0.6f, 0.0f, 1.0f);
				f.layers.push_back(worldgen::terrain::Layers(grass, 0, (1 - grass) * (1 - wet), (1 - grass) * wet));
			}
		for (int j = 0; j < f.ny; ++j)
			for (int i = 0; i < f.nx; ++i)
				f.material.push_back(f.H(i, j) > 2.4f ? 47 : 19); // grass, compact sand
		// the material: the airport ground's terrain_cb_w_4lyr with the beach's textures
		worldgen::ydr::Material material;
		{
			Bytes data;
			loader::convert::Resource res;
			const auto* tf = files.Find("ap1_01_a_ap1_gm_grnd03.ydr");
			if (!tf || !files.Read(*tf, data, error) || !loader::convert::Read(data, res, error))
				return Fail("template model: " + error);
			if (!worldgen::ydr::ReadMaterial(ue5export::drawable::View(res), ue5export::drawable::View::kRoot, 1, material, error))
				return Fail("template material: " + error);
			const std::pair<const char*, const char*> layers[] = {{"IM_Grass02V", "IM_Grass02_N"},
			    {"RSN_OS_beachhard", "NXG_RSN_OS_beachhard_n"},
			    {"NXG_RSN_OS_beachsoftprints", "NXG_RSN_OS_beachsoftprints_n"},
			    {"RSN_OS_beachhardwater", "RSN_OS_beachhardwater_n"}};
			for (int k = 0; k < 4; ++k)
			{
				material.Texture(loader::convert::Joaat(std::format("DiffuseTexture_layer{}", k)), layers[k].first);
				material.Texture(loader::convert::Joaat(std::format("BumpTexture_layer{}", k)), layers[k].second);
			}
		}
		worldgen::mapfiles::Archetype beach; // (whose texture dictionary has those textures)
		if (!worldgen::mapfiles::FindArchetype(files, "vb_35_beacha", beach, error))
			return Fail(error);
		const uint32_t txd = beach.textureDictionary;
		const fs::path out = g_options["out"];
		fs::create_directories(out);
		std::vector<worldgen::mapfiles::Archetype> archetypes;
		std::vector<worldgen::mapfiles::Entity> entities;
		const int per = static_cast<int>(std::lround(tile / cell));
		size_t triangles = 0;
		for (int tj = 0; tj * per < f.ny; ++tj)
			for (int ti = 0; ti * per < f.nx; ++ti)
			{
				const std::string tileName = std::format("{}_{}_{}", name, ti, tj);
				const float tx = x0 + (ti * per + per / 2.0f) * cell, ty = y0 + (tj * per + per / 2.0f) * cell;
				worldgen::ydr::Drawable d;
				d.name = tileName;
				d.materials.push_back(material);
				d.lods[0].push_back(worldgen::terrain::TileMesh(f, ti * per, tj * per, per, tx, ty, 5.0f));
				triangles += d.lods[0][0].indices.size() / 3;
				Bytes file;
				if (!worldgen::ydr::Write(d, file, error))
					return Fail(tileName + ": " + error);
				std::ofstream(out / (tileName + ".ydr"), std::ios::binary).write(reinterpret_cast<const char*>(file.data()), file.size());
				Bytes collision;
				if (!worldgen::ybn::Write(worldgen::terrain::TileCollision(f, ti * per, tj * per, per), collision, error))
					return Fail(tileName + " collision: " + error);
				std::ofstream(out / (name + std::format("_c_{}_{}.ybn", ti, tj)), std::ios::binary)
				    .write(reinterpret_cast<const char*>(collision.data()), collision.size());
				worldgen::mapfiles::Archetype a;
				a.name = tileName;
				a.textureDictionary = txd;
				a.lodDist = 1000;
				worldgen::ydr::Box(d, a.min, a.max);
				archetypes.push_back(a);
				worldgen::mapfiles::Entity e;
				e.archetype = loader::convert::Joaat(tileName);
				e.position = {tx, ty, 0};
				e.lodDist = 1000;
				e.radius = std::hypot(a.max[0] - a.min[0], a.max[1] - a.min[1], a.max[2] - a.min[2]) / 2;
				entities.push_back(e);
			}
		Bytes types, map;
		if (!worldgen::mapfiles::WriteTypes(files, name, archetypes, types, error) || !worldgen::mapfiles::WriteMap(files, name, entities, map, error))
			return Fail(error);
		std::ofstream(out / (name + ".ytyp"), std::ios::binary).write(reinterpret_cast<const char*>(types.data()), types.size());
		std::ofstream(out / (name + ".ymap"), std::ios::binary).write(reinterpret_cast<const char*>(map.data()), map.size());
		// read back: every model as the exporter reads it
		for (const auto& a : archetypes)
		{
			std::ifstream in(out / (a.name + ".ydr"), std::ios::binary);
			Bytes data((std::istreambuf_iterator<char>(in)), {});
			loader::convert::Resource res;
			ue5export::drawable::Model model;
			if (!loader::convert::Read(data, res, error) ||
			    !ue5export::drawable::Read(ue5export::drawable::View(res), ue5export::drawable::View::kRoot, model, error))
				return Fail(a.name + " read back: " + error);
			if (model.sections.size() != 1 || model.materials.size() != 1 || model.materials[0].textures.size() != material.textures.size())
				return Fail(a.name + " read back: not what was written");
		}
		std::printf("%s: %zu tiles, %zu triangles; archetypes %s.ytyp, map %s.ymap\n", name.c_str(), archetypes.size(), triangles, name.c_str(), name.c_str());
		return 0;
	}
	if (command == "effect-find")
	{
		// models whose shaders use an effect: --effect <name or hash>[,...] [--limit n] (the first n of each), with the
		// vertex layout of the first geometry drawn with it (declaration slots and formats)
		std::map<uint32_t, int> wanted;
		{
			std::istringstream list(g_options["effect"]);
			for (std::string n; std::getline(list, n, ',');)
				wanted[n.starts_with("0x") ? static_cast<uint32_t>(std::stoul(n, nullptr, 16)) : loader::convert::Joaat(n)] = 0;
		}
		const int limit = g_options.contains("limit") ? std::stoi(g_options["limit"]) : 3;
		using ue5export::drawable::View;
		for (const auto& [name, f] : files.Files())
		{
			if (!name.ends_with(".ydr"))
				continue;
			if (std::all_of(wanted.begin(), wanted.end(), [&](const auto& w) { return w.second >= limit; }))
				break;
			Bytes data;
			loader::convert::Resource res;
			if (!files.Read(f, data, error) || !loader::convert::Read(data, res, error))
				continue;
			const View view(res);
			const uint64_t group = view.Get<uint64_t>(View::kRoot + 0x10);
			const uint64_t shaders = view.Get<uint64_t>(group + 0x10);
			const uint16_t count = view.Get<uint16_t>(group + 0x18);
			for (uint16_t s = 0; s < count; ++s)
			{
				const uint32_t effect = view.Get<uint32_t>(view.Get<uint64_t>(shaders + 8u * s));
				const auto it = wanted.find(effect);
				if (it == wanted.end() || it->second >= limit)
					continue;
				++it->second;
				// the first geometry of the high level of detail drawn with this shader
				std::string layout;
				const uint64_t list = view.Get<uint64_t>(View::kRoot + 0x50);
				const uint64_t models = view.Get<uint64_t>(list);
				for (uint16_t m = 0; m < view.Get<uint16_t>(list + 8) && layout.empty(); ++m)
				{
					const uint64_t model = view.Get<uint64_t>(models + 8u * m);
					const uint64_t geometries = view.Get<uint64_t>(model + 0x08), mapping = view.Get<uint64_t>(model + 0x20);
					for (uint16_t g = 0; g < view.Get<uint16_t>(model + 0x10) && layout.empty(); ++g)
					{
						if (view.Get<uint16_t>(mapping + 2u * g) != s)
							continue;
						const uint64_t vb = view.Get<uint64_t>(view.Get<uint64_t>(geometries + 8u * g) + 0x18);
						const uint8_t* decl = view.At(view.Get<uint64_t>(vb + 0x38), 0x140);
						if (!decl)
							continue;
						layout = std::format("stride {}:", view.Get<uint16_t>(vb + 0x0C) & 0xFFF);
						for (int slot = 0; slot < 36; ++slot)
							if (decl[0x104 + slot])
								layout += std::format(" {}/{}", slot, decl[0x104 + slot]);
					}
				}
				std::printf("%08x %s shader %u: %s\n", effect, name.c_str(), s, layout.c_str());
			}
		}
		return 0;
	}
	if (command == "alias")
	{
		// an archetype file of aliases: --models a,b,... (game models) --name <ytyp name> --out <dir>: "<name>_<model>" each,
		// the model's own fields and model (assetName) under the new name
		std::vector<worldgen::mapfiles::Archetype> list;
		std::istringstream models(g_options["models"]);
		for (std::string m; std::getline(models, m, ',');)
		{
			worldgen::mapfiles::Archetype a;
			if (!worldgen::mapfiles::FindArchetype(files, m, a, error))
				return Fail(error);
			a.assetName = loader::convert::Joaat(m);
			a.name = g_options["name"] + "_" + m;
			list.push_back(a);
			std::printf("%s -> %s (txd %08x)\n", m.c_str(), a.name.c_str(), a.textureDictionary);
		}
		Bytes types;
		if (!worldgen::mapfiles::WriteTypes(files, g_options["name"], list, types, error))
			return Fail(error);
		fs::create_directories(g_options["out"]);
		std::ofstream(fs::path(g_options["out"]) / (g_options["name"] + ".ytyp"), std::ios::binary).write(reinterpret_cast<const char*>(types.data()), types.size());
		return 0;
	}
	if (command == "build")
	{
		// a world description -> its pack's files (build.hpp): --world <world.json> --out <dir> [--zones <mod dir>] [--name n]
		// [--area x0,y0,x1,y1] [--tile m] [--cell m] [--navmesh 0] [--paths 0] [--scan <lidar.py rasters> [--aerial <aerial.py
		// tiles>] [--datum m]: measured ground and buildings of our own] [--waves s: the sea's swell round it, 1 the open ocean's]
		// [--phototex <dir>: the street panoramas on the walls, tools/worldgen/phototex.py's]
		if (!g_options.contains("world") || !g_options.contains("out"))
			return Fail("--world <world.json> --out <dir> are needed");
		worldgen::world::World w;
		if (!worldgen::world::Load(g_options["world"], w, error))
			return Fail(error);
		// (the world editor's changes on top: --edits <edits.json>)
		if (g_options.contains("edits") && !worldgen::world::ApplyEdits(w, g_options["edits"], error))
			return Fail(error);
		// (the property rolls first: their land use is the parcel's own)
		if (g_options.contains("parcels"))
		{
			worldgen::world::ParcelStats ps;
			if (!worldgen::world::ApplyParcels(w, g_options["parcels"], ps, error))
				return Fail(error);
			std::printf("parcels: %zu footprints on vacant land left out, %zu buildings' kind and %zu heights by the property rolls\n", ps.vacant,
			    ps.retyped, ps.raised);
		}
		std::printf("buildings: %zu of the map's \"yes\" in commercial or industrial land taken as such\n", worldgen::world::KindsByLandUse(w));
		worldgen::build::Options o;
		o.out = g_options["out"];
		o.name = g_options.contains("name") ? g_options["name"] : w.name;
		if (g_options.contains("zones"))
			o.zonesOut = g_options["zones"];
		if (g_options.contains("area"))
		{
			char c = 0;
			std::istringstream(g_options["area"]) >> o.area[0] >> c >> o.area[1] >> c >> o.area[2] >> c >> o.area[3];
		}
		if (g_options.contains("tile"))
			o.tile = std::stof(g_options["tile"]);
		if (g_options.contains("cell"))
			o.cell = std::stof(g_options["cell"]);
		if (g_options.contains("scan"))
			o.scan = g_options["scan"];
		if (g_options.contains("poles"))
			o.poles = g_options["poles"];
		if (g_options.contains("parcels"))
			o.parcels = g_options["parcels"];
		if (g_options.contains("aerial"))
			o.aerial = g_options["aerial"];
		if (g_options.contains("landcover"))
			o.landcover = g_options["landcover"];
		if (g_options.contains("ground"))
			o.ground = g_options["ground"];
		if (g_options.contains("photo"))
			o.photo = g_options["photo"];
		if (g_options.contains("facades"))
			o.facades = g_options["facades"];
		if (g_options.contains("phototex"))
			o.phototex = g_options["phototex"];
		o.surfaceOnly = g_options.contains("surface-only") && g_options["surface-only"] != "0";
		o.photoWalls = !g_options.contains("photo-walls") || g_options["photo-walls"] != "0";
		if (g_options.contains("climate"))
			o.climate = g_options["climate"];
		if (g_options.contains("datum"))
			o.datum = std::stof(g_options["datum"]);
		if (g_options.contains("waves"))
			o.waves = std::stof(g_options["waves"]);
		if (g_options.contains("open"))
			o.open = g_options["open"]; // (the box's edges a neighbouring sector goes on from: n, s, e, w)
		if (g_options.contains("core"))
		{
			char c = 0;
			std::istringstream(g_options["core"]) >> o.core[0] >> c >> o.core[1] >> c >> o.core[2] >> c >> o.core[3];
		}
		if (g_options.contains("roads-obj"))
			o.roadsObj = g_options["roads-obj"];
		o.lods = !g_options.contains("lods") || g_options["lods"] != "0";
		o.navmesh = !g_options.contains("navmesh") || g_options["navmesh"] != "0";
		o.navmeshNearOnly = g_options.contains("navmesh") && g_options["navmesh"] == "near"; // (--navmesh near: only where people go)
		o.paths = !g_options.contains("paths") || g_options["paths"] != "0";
		std::vector<worldgen::catalog::Item> catalog;
		if (g_options.contains("catalog") && !worldgen::catalog::Load(g_options["catalog"], catalog, error))
			return Fail(error);
		worldgen::build::Stats s;
		const auto start = std::chrono::steady_clock::now();
		if (!worldgen::build::Run(files, w, catalog, o, s, error))
			return Fail(error);
		std::printf("%s: %zu tiles (%zu terrain triangles), %zu road edges (%zu collision triangles), %zu navmeshes, %zu path nodes in %zu regions, "
		            "%zu zones (%.0f s)\n",
		    o.name.c_str(), s.tiles, s.terrainTriangles, s.roadEdges, s.roadTriangles, s.navmeshes, s.pathNodes, s.regions, s.zones,
		    std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count());
		return 0;
	}
	if (command == "osm")
	{
		// OpenStreetMap data (tools/osm_fetch.py's folder) -> a world description in game coordinates: --osm <dir> --out
		// <world dir> [--at x,y: where the box's south-west corner goes (default 4300,-3000)] [--name n]; writes world.json
		// and world.png (8 m a pixel: water, land use, buildings, roads)
		// (or --pbf <file.osm.pbf> --box south,west,north,east: a binary extract, read for that box)
		if ((!g_options.contains("osm") && !g_options.contains("pbf")) || !g_options.contains("out"))
			return Fail("--osm <dir> (or --pbf <file> --box s,w,n,e) and --out <dir> are needed");
		worldgen::osm::Data data;
		if (g_options.contains("pbf"))
		{
			double s = 0, w0 = 0, n = 0, e = 0;
			char c = 0;
			std::istringstream(g_options["box"]) >> s >> c >> w0 >> c >> n >> c >> e;
			if (n <= s || e <= w0)
				return Fail("--box south,west,north,east is needed with --pbf");
			const auto start = std::chrono::steady_clock::now();
			if (!worldgen::pbf::Load(g_options["pbf"], s, w0, n, e, 0.05, data, error))
				return Fail(error);
			std::printf("extract: %zu nodes, %zu ways, %zu relations kept (%.0f s)\n", data.nodes.size(), data.ways.size(), data.relations.size(),
			    std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count());
		}
		else if (!worldgen::osm::Load(g_options["osm"], data, error))
			return Fail(error);
		double ax = 4300, ay = -3000;
		if (g_options.contains("at"))
		{
			char comma = 0;
			std::istringstream(g_options["at"]) >> ax >> comma >> ay;
		}
		auto projection = worldgen::world::Projection::Around(data.south, data.west, data.north, data.east, ax, ay);
		// (--projection lat0,lon0,x0,y0,kx,ky: one given - tools/worldgen/sectors.py's, the same for every land of Florida, each
		// land a translation of it (x0, y0): a land's band of its neighbour's is then the neighbour's own, only moved)
		if (g_options.contains("projection"))
		{
			char c = 0;
			auto& p = projection;
			std::istringstream(g_options["projection"]) >> p.lat0 >> c >> p.lon0 >> c >> p.x0 >> c >> p.y0 >> c >> p.kx >> c >> p.ky;
			if (p.kx <= 0 || p.ky <= 0)
				return Fail("--projection lat0,lon0,x0,y0,kx,ky");
		}
		worldgen::world::World w;
		w.name = g_options.contains("name") ? g_options["name"] : "world";
		if (!worldgen::world::FromOsm(data, projection, w, error))
			return Fail(error);
		const fs::path out = g_options["out"];
		fs::create_directories(out);
		if (!worldgen::world::Save(w, out / "world.json", error))
			return Fail(error);
		std::printf("%s: box %.0f,%.0f .. %.0f,%.0f; %zu coastlines, %zu areas, %zu waterways, %zu roads, %zu buildings, %zu points\n", w.name.c_str(), w.min[0],
		    w.min[1], w.max[0], w.max[1], w.coastlines.size(), w.areas.size(), w.waterways.size(), w.roads.size(), w.buildings.size(), w.points.size());
		// the picture
		worldgen::raster::Grid<uint8_t> g;
		g.Init(w.min[0], w.min[1], w.max[0], w.max[1], 8.0f, 0);
		enum : uint8_t { kWater, kLand, kPark, kBeach, kWood, kParking, kBuilding, kMinor, kMajor, kMotorway };
		worldgen::raster::ClassifyCoast(g, w.coastlines, 160.0f);
		const auto fill = [&](const std::string& kind, uint8_t value) {
			for (const auto& a : w.areas)
				if (a.kind == kind)
				{
					auto rings = a.outer;
					rings.insert(rings.end(), a.inner.begin(), a.inner.end());
					worldgen::raster::FillPolygon(g, rings, value);
				}
		};
		for (const char* k : {"park", "grass", "cemetery", "pitch"})
			fill(k, kPark);
		fill("wood", kWood);
		fill("beach", kBeach);
		fill("parking", kParking);
		fill("water", kWater);
		for (const auto& ww : w.waterways)
			worldgen::raster::DrawLine(g, ww.points, ww.width / 2, static_cast<uint8_t>(kWater));
		for (const auto& b : w.buildings)
			worldgen::raster::FillPolygon(g, {b.footprint}, static_cast<uint8_t>(kBuilding));
		for (const auto& r : w.roads)
		{
			const float half = (r.forward + r.backward) * r.laneWidth / 2;
			const uint8_t c = r.kind.starts_with("motorway") || r.kind.starts_with("trunk") ? kMotorway
			                  : r.kind.starts_with("primary") || r.kind.starts_with("secondary") ? kMajor
			                                                                                     : kMinor;
			worldgen::raster::DrawLine(g, r.points, std::max(half, 4.0f), c);
		}
		const std::array<uint8_t, 3> colours[] = {{40, 90, 160}, {205, 195, 170}, {120, 170, 90}, {235, 220, 160}, {70, 120, 60}, {150, 150, 150},
		    {110, 100, 95}, {250, 250, 250}, {250, 210, 120}, {230, 120, 90}};
		std::vector<std::array<uint8_t, 3>> rgb(g.v.size());
		for (size_t k = 0; k < g.v.size(); ++k)
			rgb[k] = colours[g.v[k]];
		if (!worldgen::raster::WritePng(out / "world.png", g.nx, g.ny, rgb, error))
			return Fail(error);
		std::printf("world.png: %d x %d\n", g.nx, g.ny);
		return 0;
	}
	if (command == "survey")
	{
		// a big extract read once, counted on the sectors' grid (tools/worldgen/sectors.py): --pbf <file.osm.pbf> --box
		// south,west,north,east --grid lat0,lon0,dlat,dlon (cell (0, 0)'s south-west corner, a cell's size) --out <grid.tsv>:
		// a line a cell with anything in it - column, row, coastline pieces, roads, buildings, water areas
		double s = 0, w0 = 0, n = 0, e = 0, lat0 = 0, lon0 = 0, dlat = 0, dlon = 0;
		char c = 0;
		std::istringstream(g_options["box"]) >> s >> c >> w0 >> c >> n >> c >> e;
		std::istringstream(g_options["grid"]) >> lat0 >> c >> lon0 >> c >> dlat >> c >> dlon;
		if (!g_options.contains("pbf") || !g_options.contains("out") || n <= s || e <= w0 || dlat <= 0 || dlon <= 0)
			return Fail("--pbf <file> --box s,w,n,e --grid lat0,lon0,dlat,dlon --out <grid.tsv> are needed");
		const auto start = std::chrono::steady_clock::now();
		worldgen::osm::Data data;
		if (!worldgen::pbf::Load(g_options["pbf"], s, w0, n, e, 0, data, error))
			return Fail(error);
		std::printf("extract: %zu nodes, %zu ways (%.0f s)\n", data.nodes.size(), data.ways.size(),
		    std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count());
		struct Count
		{
			int coast = 0, roads = 0, buildings = 0, water = 0;
		};
		std::map<std::pair<int, int>, Count> cells;
		const auto cellOf = [&](const worldgen::osm::Node& p) {
			return std::make_pair(static_cast<int>(std::floor((p.lon - lon0) / dlon)), static_cast<int>(std::floor((p.lat - lat0) / dlat)));
		};
		for (const auto& [id, way] : data.ways)
		{
			const bool coast = worldgen::osm::Tag(way.tags, "natural") == "coastline";
			const bool road = !worldgen::osm::Tag(way.tags, "highway").empty();
			const bool building = !worldgen::osm::Tag(way.tags, "building").empty();
			const bool water = worldgen::osm::Tag(way.tags, "natural") == "water";
			if (!coast && !road && !building && !water)
				continue;
			// each cell the way passes through once (its pieces sampled every 1/8 of a cell: a coastline's straight runs
			// can be long)
			std::set<std::pair<int, int>> through;
			const worldgen::osm::Node* last = nullptr;
			for (const int64_t ref : way.nodes)
			{
				const auto it = data.nodes.find(ref);
				if (it == data.nodes.end())
					continue;
				const auto& p = it->second;
				if (last)
				{
					const int steps = static_cast<int>(std::max(std::abs(p.lat - last->lat) / dlat, std::abs(p.lon - last->lon) / dlon) * 8) + 1;
					for (int k = 1; k < steps; ++k)
						through.insert(cellOf({last->lat + (p.lat - last->lat) * k / steps, last->lon + (p.lon - last->lon) * k / steps}));
				}
				through.insert(cellOf(p));
				last = &p;
				if (building || water)
					break; // (a building or a lake: where it starts)
			}
			for (const auto& at : through)
			{
				Count& k = cells[at];
				k.coast += coast, k.roads += road, k.buildings += building, k.water += water;
			}
		}
		std::ofstream out(g_options["out"]);
		out << "column\trow\tcoast\troads\tbuildings\twater\n";
		for (const auto& [at, k] : cells)
			out << at.first << '\t' << at.second << '\t' << k.coast << '\t' << k.roads << '\t' << k.buildings << '\t' << k.water << '\n';
		std::printf("%zu cells with something in them (%.0f s)\n", cells.size(), std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count());
		return 0;
	}
	if (command == "catalog")
	{
		// the asset catalog (catalog.hpp): --out <file.tsv>
		if (!g_options.contains("out"))
			return Fail("--out <file.tsv> is needed");
		std::vector<worldgen::catalog::Item> items;
		if (!worldgen::catalog::Build(files, items, error) || !worldgen::catalog::Save(items, g_options["out"], error))
			return Fail(error);
		std::map<std::string, int> kinds;
		for (const auto& it : items)
			++kinds[worldgen::catalog::Name(it.kind)];
		std::printf("%zu archetypes:", items.size());
		for (const auto& [k, n] : kinds)
			std::printf(" %s %d", k.c_str(), n);
		std::printf("\n");
		return 0;
	}
	if (command == "zones")
	{
		// population zones for new land: --zones <file> (lines "name, x0, y0, z0, x1, y1, z1, label, like": a box, the text
		// label it shows, and the game zone whose population it takes) --out <mod folder>. Writes popzone.ipl (the boxes,
		// merged into the game's by the loader) and zonebind.ymt (copies of those zones' bindings under the new names).
		if (!g_options.contains("zones") || !g_options.contains("out"))
			return Fail("--zones <file> --out <dir> are needed");
		Bytes data;
		const auto* f = files.Find("zonebind.ymt");
		if (!f || !files.Read(*f, data, error))
			return Fail("zonebind.ymt: " + error);
		loader::convert::pso::Entries game, ours;
		if (!game.Read(std::string(data.begin(), data.end()), error) || !ours.Read(std::string(data.begin(), data.end()), error))
			return Fail("zonebind.ymt: " + error);
		ours.Clear();
		std::string ipl = "zone\n";
		std::ifstream in(g_options["zones"]);
		int count = 0;
		for (std::string line; std::getline(in, line);)
		{
			if (line.empty() || line[0] == '#')
				continue;
			std::vector<std::string> fields;
			std::istringstream words(line);
			for (std::string w; std::getline(words, w, ',');)
			{
				w.erase(0, w.find_first_not_of(" \t"));
				w.erase(w.find_last_not_of(" \t\r") + 1);
				fields.push_back(w);
			}
			if (fields.size() != 9)
				return Fail("not a zone line: " + line);
			const int like = game.Find(fields[8]);
			if (like < 0)
				return Fail("no game zone " + fields[8]);
			ours.Put(game, like, fields[0]);
			ipl += std::format("{}, {}, {}, {}, {}, {}, {}, {}, 0\n", fields[0], fields[1], fields[2], fields[3], fields[4], fields[5], fields[6], fields[7]);
			++count;
		}
		ipl += "end\n";
		std::string binds;
		if (!ours.Write(binds, error))
			return Fail(error);
		const fs::path out = g_options["out"];
		fs::create_directories(out);
		std::ofstream(out / "popzone.ipl", std::ios::binary) << ipl;
		std::ofstream(out / "zonebind.ymt", std::ios::binary).write(binds.data(), static_cast<std::streamsize>(binds.size()));
		// read back
		loader::convert::pso::Entries back;
		if (!back.Read(binds, error) || back.Size() != static_cast<size_t>(count))
			return Fail("zonebind.ymt read back: " + error);
		std::printf("%d zones: popzone.ipl, zonebind.ymt (%zu bytes)\n", count, binds.size());
		return 0;
	}
	if (command == "pso-dump")
	{
		// a PSO file's structures (--name <file>): the root's fields, and the first --limit entries of its arrays of
		// structures (every field's value as a u32; strings and hashes as they are)
		Bytes data;
		ue5export::pso::Pso pso;
		const auto* f = files.Find(g_options["name"]);
		if (!f || !files.Read(*f, data, error) || !pso.Read(data, error))
			return Fail("cannot read " + g_options["name"] + ": " + error);
		const int limit = g_options.contains("limit") ? std::stoi(g_options["limit"]) : 3;
		const ue5export::pso::Ref root = pso.Root();
		std::printf("root %08x, %zu bytes\n", root.type->name, static_cast<size_t>(root.type->size));
		for (const auto& field : root.type->fields)
		{
			std::printf("  field %08x type %#x subtype %u offset %u ref %#x\n", field.name, field.type, field.subtype, field.offset, field.ref);
			if (field.type != ue5export::pso::kArray)
				continue;
			const auto items = root.Array(field.name);
			std::printf("    %zu entries\n", items.size());
			for (size_t i = 0; i < items.size() && static_cast<int>(i) < limit; ++i)
			{
				std::printf("    [%zu] %08x:", i, items[i].type->name);
				for (const auto& sub : items[i].type->fields)
					if (sub.type == ue5export::pso::kString)
						std::printf(" %08x=\"%s\"(s%u %#x)", sub.name, items[i].String(sub.name).c_str(), sub.subtype, items[i].U32(sub.name));
					else
						std::printf(" %08x=%u", sub.name, items[i].U32(sub.name));
				std::printf("\n");
			}
		}
		return 0;
	}
	if (command == "archetype-find")
	{
		// where archetypes are defined: --name a,b,... (every archetype file is read); their texture dictionaries etc.
		std::set<uint32_t> wanted;
		{
			std::istringstream list(g_options["name"]);
			for (std::string n; std::getline(list, n, ',');)
				wanted.insert(ue5export::meta::Hash(n.c_str()));
		}
		using ue5export::meta::Hash;
		for (const auto& [name, f] : files.Files())
		{
			if (!name.ends_with(".ytyp"))
				continue;
			Bytes data;
			ue5export::meta::Meta m;
			if (!files.Read(f, data, error) || !m.Read(data, error))
				continue;
			for (const ue5export::meta::Ref& a : m.Root().Array(Hash("archetypes")))
			{
				if (!a || !wanted.contains(a.Get<uint32_t>(Hash("name"))))
					continue;
				const auto lo = a.Get<std::array<float, 4>>(Hash("bbMin")), hi = a.Get<std::array<float, 4>>(Hash("bbMax"));
				std::printf("%s: name %#x txd %#x drawable dictionary %#x physics %#x asset %#x type %u flags %#x lodDist %.0f hdTextureDist %.0f "
				            "box %.1f %.1f %.1f .. %.1f %.1f %.1f\n",
				    name.c_str(), a.Get<uint32_t>(Hash("name")), a.Get<uint32_t>(Hash("textureDictionary")), a.Get<uint32_t>(Hash("drawableDictionary")),
				    a.Get<uint32_t>(Hash("physicsDictionary")), a.Get<uint32_t>(Hash("assetName")), a.Get<uint32_t>(Hash("assetType")),
				    a.Get<uint32_t>(Hash("flags")), a.Get<float>(Hash("lodDist")), a.Get<float>(Hash("hdTextureDist")), lo[0], lo[1], lo[2], hi[0], hi[1], hi[2]);
			}
		}
		return 0;
	}
	if (command == "ymap-dump")
	{
		// a map file's header fields and its first entities (to see what the game's own files carry)
		Bytes data;
		ue5export::meta::Meta m;
		if (g_options.contains("file"))
		{
			std::ifstream in(g_options["file"], std::ios::binary);
			data.assign(std::istreambuf_iterator<char>(in), {});
		}
		else if (const auto* f = files.Find(g_options["name"]); !f || !files.Read(*f, data, error))
			return Fail("cannot read " + g_options["name"] + " " + error);
		if (!m.Read(data, error))
			return Fail("not a map file: " + error);
		const ue5export::meta::Ref root = m.Root();
		using ue5export::meta::Hash;
		for (const char* field : {"name", "parent", "flags", "contentFlags"})
			std::printf("%s %#x\n", field, root.Get<uint32_t>(Hash(field)));
		for (const char* field : {"streamingExtentsMin", "streamingExtentsMax", "entitiesExtentsMin", "entitiesExtentsMax"})
		{
			const auto v = root.Get<std::array<float, 4>>(Hash(field));
			std::printf("%s %.1f %.1f %.1f\n", field, v[0], v[1], v[2]);
		}
		// (--archetype a: every entity of that archetype, else the first 12)
		const uint32_t only = g_options.contains("archetype") ? Hash(g_options["archetype"].c_str()) : 0;
		int shown = 0;
		for (const ue5export::meta::Ref& e : root.Array(Hash("entities")))
		{
			if (!e || (only ? e.Get<uint32_t>(Hash("archetypeName")) != only : shown++ >= 12))
				continue;
			const auto p = e.Get<std::array<float, 4>>(Hash("position"));
			const auto q = e.Get<std::array<float, 4>>(Hash("rotation"));
			std::printf("entity %#x flags %#x guid %#x pos %.1f %.1f %.1f rot %.3f %.3f %.3f %.3f scale %.2f %.2f parent %d lod %.0f child %.0f level %u children %u priority %u ao %u %u tint %u\n",
			    e.Get<uint32_t>(Hash("archetypeName")), e.Get<uint32_t>(Hash("flags")), e.Get<uint32_t>(Hash("guid")), p[0], p[1], p[2], q[0], q[1], q[2],
			    q[3], e.Get<float>(Hash("scaleXY")), e.Get<float>(Hash("scaleZ")), e.Get<int32_t>(Hash("parentIndex")), e.Get<float>(Hash("lodDist")),
			    e.Get<float>(Hash("childLodDist")), e.Get<uint32_t>(Hash("lodLevel")), e.Get<uint32_t>(Hash("numChildren")),
			    e.Get<uint32_t>(Hash("priorityLevel")), e.Get<uint32_t>(Hash("ambientOcclusionMultiplier")),
			    e.Get<uint32_t>(Hash("artificialAmbientOcclusion")), e.Get<uint32_t>(Hash("tintValue")));
		}
		return 0;
	}
	if (command == "entities")
	{
		// every map entity a build made (--in <its output folder>, its .ymap files) in the real world's place (--origin x0,y0:
		// the projection's, world.json - taken off): --out <file.tsv> (map, archetype, x, y, z, rotation, scale, lodDist), for
		// comparing two lands' shared band (tools/worldgen/banddiff.py)
		if (!g_options.contains("in") || !g_options.contains("out"))
			return Fail("--in <build folder> --out <file.tsv> [--origin x0,y0] are needed");
		double ox = 0, oy = 0;
		if (g_options.contains("origin"))
		{
			char c = 0;
			std::istringstream(g_options["origin"]) >> ox >> c >> oy;
		}
		std::ofstream out(g_options["out"]);
		out << "map\tarchetype\tx\ty\tz\tqx\tqy\tqz\tqw\tscaleXY\tscaleZ\tlodDist\tbox\n";
		using ue5export::meta::Hash;
		size_t maps = 0, entities = 0;
		std::error_code ec;
		// (the build's own archetypes: their boxes - "-" for the game's -, what an entity's model is like when its name is
		// the land's)
		std::map<uint32_t, std::string> boxes;
		for (const auto& entry : fs::directory_iterator(g_options["in"], ec))
		{
			if (entry.path().extension() != ".ytyp")
				continue;
			std::ifstream in(entry.path(), std::ios::binary);
			Bytes data(std::istreambuf_iterator<char>(in), {});
			ue5export::meta::Meta m;
			if (data.empty() || !m.Read(data, error))
				continue;
			for (const ue5export::meta::Ref& a : m.Root().Array(Hash("archetypes")))
				if (a)
				{
					const auto lo = a.Get<std::array<float, 4>>(Hash("bbMin")), hi = a.Get<std::array<float, 4>>(Hash("bbMax"));
					boxes[a.Get<uint32_t>(Hash("name"))] = std::format("{:.2f},{:.2f},{:.2f},{:.2f},{:.2f},{:.2f}", lo[0], lo[1], lo[2], hi[0], hi[1], hi[2]);
				}
		}
		for (const auto& entry : fs::directory_iterator(g_options["in"], ec))
		{
			if (entry.path().extension() != ".ymap")
				continue;
			std::ifstream in(entry.path(), std::ios::binary);
			Bytes data(std::istreambuf_iterator<char>(in), {});
			ue5export::meta::Meta m;
			if (data.empty() || !m.Read(data, error))
				continue;
			++maps;
			const std::string name = entry.path().stem().string();
			for (const ue5export::meta::Ref& e : m.Root().Array(Hash("entities")))
			{
				if (!e)
					continue;
				const auto p = e.Get<std::array<float, 4>>(Hash("position"));
				const auto q = e.Get<std::array<float, 4>>(Hash("rotation"));
				const uint32_t archetype = e.Get<uint32_t>(Hash("archetypeName"));
				const auto box = boxes.find(archetype);
				out << std::format("{}\t{:08x}\t{:.3f}\t{:.3f}\t{:.3f}\t{:.4f}\t{:.4f}\t{:.4f}\t{:.4f}\t{:.3f}\t{:.3f}\t{:.0f}\t{}\n", name, archetype, p[0] - ox,
				    p[1] - oy, p[2], q[0], q[1], q[2], q[3], e.Get<float>(Hash("scaleXY")), e.Get<float>(Hash("scaleZ")), e.Get<float>(Hash("lodDist")),
				    box != boxes.end() ? box->second : "-");
				++entities;
			}
		}
		std::printf("%zu maps, %zu entities\n", maps, entities);
		return 0;
	}
	if (command == "cargen-dump")
	{
		// a map file's car generators (parked cars): --name <file.ymap> (the game's) or --file <path> (one on disk, ours):
		// CCarGen's layout, each one's fields
		Bytes data;
		ue5export::meta::Meta m;
		if (g_options.contains("file"))
		{
			std::ifstream in(g_options["file"], std::ios::binary);
			data.assign(std::istreambuf_iterator<char>(in), {});
			if (data.empty() || !m.Read(data, error))
				return Fail("cannot read " + g_options["file"] + " " + error);
		}
		else if (const auto* f = files.Find(g_options["name"]); !f || !files.Read(*f, data, error) || !m.Read(data, error))
			return Fail("cannot read " + g_options["name"] + " " + error);
		using ue5export::meta::Hash;
		if (const auto* st = m.Find(Hash("CCarGen")))
		{
			std::printf("CCarGen: size %u\n", st->size);
			for (const auto& fl : st->fields)
				std::printf("  field %08x at %d type %u\n", fl.name, fl.offset, fl.type);
		}
		for (const auto& g : m.Root().Array(Hash("carGenerators")))
		{
			const auto p = g.Get<std::array<float, 4>>(Hash("position"));
			std::printf("cargen at %.1f %.1f %.1f orient %.2f %.2f perp %.2f model %08x flags %08x colours %d %d %d %d pop %08x livery %d\n", p[0], p[1], p[2],
			    g.Get<float>(Hash("orientX")), g.Get<float>(Hash("orientY")), g.Get<float>(Hash("perpendicularLength")), g.Get<uint32_t>(Hash("carModel")),
			    g.Get<uint32_t>(Hash("flags")), g.Get<int32_t>(Hash("bodyColorRemap1")), g.Get<int32_t>(Hash("bodyColorRemap2")),
			    g.Get<int32_t>(Hash("bodyColorRemap3")), g.Get<int32_t>(Hash("bodyColorRemap4")), g.Get<uint32_t>(Hash("popGroup")),
			    static_cast<int>(g.Get<int8_t>(Hash("livery"))));
		}
		return 0;
	}
	if (command == "grass-dump")
	{
		// a map file's grass batches (instancedData.GrassInstanceList): --name <file.ymap>: the structures' layouts, each
		// batch's archetype, distances, box, count and first instances
		Bytes data;
		ue5export::meta::Meta m;
		if (const auto* f = files.Find(g_options["name"]); !f || !files.Read(*f, data, error) || !m.Read(data, error))
			return Fail("cannot read " + g_options["name"] + " " + error);
		using ue5export::meta::Hash;
		for (const char* s : {"CMapData", "rage__fwInstancedMapData", "rage__fwGrassInstanceListDef", "rage__fwGrassInstanceListDef__InstanceData", "rage__spdAABB"})
			if (const auto* st = m.Find(Hash(s)))
			{
				std::printf("%s: size %u\n", s, st->size);
				for (const auto& fl : st->fields)
					std::printf("  field %08x at %d type %u\n", fl.name, fl.offset, fl.type);
			}
		const auto inst = m.Root().Struct(Hash("instancedData"), Hash("rage__fwInstancedMapData"));
		if (!inst)
			return Fail("no instancedData");
		for (const auto& g : inst.Array(Hash("GrassInstanceList")))
		{
			const auto box = g.Struct(Hash("BatchAABB"), Hash("rage__spdAABB"));
			const auto lo = box.Get<std::array<float, 4>>(Hash("min")), hi = box.Get<std::array<float, 4>>(Hash("max"));
			const auto scale = g.Get<std::array<float, 3>>(Hash("ScaleRange"));
			const auto list = g.Array(Hash("InstanceList"));
			std::printf("batch %08x lod %u fade start %.1f inst fade %.2f orient %.2f scale %.2f..%.2f (%.2f) box %.1f %.1f %.1f .. %.1f %.1f %.1f: %zu\n",
			    g.Get<uint32_t>(Hash("archetypeName")), g.Get<uint32_t>(Hash("lodDist")), g.Get<float>(Hash("LodFadeStartDist")),
			    g.Get<float>(Hash("LodInstFadeRange")), g.Get<float>(Hash("OrientToTerrain")), scale[0], scale[1], scale[2], lo[0], lo[1], lo[2], hi[0], hi[1],
			    hi[2], list.size());
			for (size_t k = 0; k < std::min<size_t>(list.size(), 3); ++k)
			{
				std::printf("   ");
				for (int b = 0; b < 16; ++b)
					std::printf(" %02x", m.At(list[k].at)[b]);
				std::printf("\n");
			}
		}
		return 0;
	}
	if (command == "meta-roundtrip")
	{
		// metadata files (--name pattern: ymap, ytyp) written again with our writer from their own blocks (the file its own
		// template) and read back: the same blocks, in the same order
		const std::string pattern = g_options.contains("name") ? g_options["name"] : "*.ymap";
		const auto match = [&](const std::string& name) {
			const size_t star = pattern.find('*');
			if (star == std::string::npos)
				return name == pattern;
			const std::string head = pattern.substr(0, star), tail = pattern.substr(star + 1);
			return name.size() >= head.size() + tail.size() && name.starts_with(head) && name.ends_with(tail);
		};
		// a metadata resource's blocks: (structure, data), the root block's number
		const auto blocks = [](const Bytes& file, std::vector<std::pair<uint32_t, Bytes>>& out, uint32_t& rootBlock, std::string& why) {
			loader::convert::Resource r;
			if (!loader::convert::Read(file, r, why))
				return false;
			Bytes v = r.virtualBlock;
			const size_t virtualSize = v.size();
			v.insert(v.end(), r.physicalBlock.begin(), r.physicalBlock.end());
			rootBlock = loader::convert::Get<uint32_t>(v, 0x1C);
			const uint64_t list = loader::convert::Get<uint64_t>(v, 0x30);
			const uint16_t count = loader::convert::Get<uint16_t>(v, 0x4C);
			if ((list >> 28) != 5)
				return why = "no block list", false;
			for (uint16_t i = 0; i < count; ++i)
			{
				const size_t at = (list & 0x0FFFFFFF) + 16u * i;
				const uint32_t structure = loader::convert::Get<uint32_t>(v, at), size = loader::convert::Get<uint32_t>(v, at + 4);
				const uint64_t p = loader::convert::Get<uint64_t>(v, at + 8);
				const size_t data = (p >> 28) == 5 ? (p & 0x0FFFFFFF) : (p >> 28) == 6 ? virtualSize + (p & 0x0FFFFFFF) : SIZE_MAX;
				if (data == SIZE_MAX || data + size > v.size())
					return why = "a block outside the resource", false;
				out.emplace_back(structure, Bytes(v.begin() + data, v.begin() + data + size));
			}
			return true;
		};
		size_t count = 0, bad = 0, skipped = 0;
		for (const auto& [name, f] : files.Files())
		{
			if (!match(name))
				continue;
			Bytes data, written;
			std::vector<std::pair<uint32_t, Bytes>> before, after;
			uint32_t root = 0, rootAgain = 0;
			std::string why;
			worldgen::meta::Writer w;
			if (!files.Read(f, data, error) || !blocks(data, before, root, why) || !w.Template(data, why))
				why = "read: " + (why.empty() ? error : why);
			else
			{
				bool big = false;
				for (auto& [structure, bytes] : before)
				{
					big |= bytes.size() > worldgen::Pages::kSize;
					w.AddBlock(structure, bytes);
				}
				if (big)
				{
					++skipped;
					continue;
				}
				if (!w.Write(root, written, why) || !blocks(written, after, rootAgain, why))
					why = "again: " + why;
				else if (root != rootAgain || before != after)
					why = "blocks differ";
			}
			if (!why.empty() && ++bad <= 20)
				std::printf("%s: %s\n", name.c_str(), why.c_str());
			++count;
		}
		std::printf("%zu files: %zu differ after writing them again (%zu with a block over 16 KB skipped)\n", count, bad, skipped);
		return bad ? 1 : 0;
	}
	if (command == "ynv-reindex")
	{
		// every navmesh renumbered for a wider grid (--width, default 147; the game's is 100): index = y * width + x for its
		// own index, the adjacency table, each polygon's and each special link's (two 14-bit indices at +0x18)
		if (!g_options.contains("out"))
			return Fail("--out <dir> is needed");
		const uint32_t width = g_options.contains("width") ? static_cast<uint32_t>(std::stoul(g_options["width"])) : 147;
		const auto remap = [&](uint32_t index) -> uint32_t {
			if (index >= 10000)
				return index; // (not a square: none, 0x3FFF; dynamic navmeshes are not in files)
			return index / 100 * width + index % 100;
		};
		const fs::path out = g_options["out"];
		fs::create_directories(out);
		size_t count = 0;
		for (const auto& [name, f] : files.Files())
		{
			if (!name.starts_with("navmesh") || !name.ends_with(".ynv"))
				continue;
			Bytes data, written;
			worldgen::ynv::Navmesh m;
			if (!files.Read(f, data, error) || !worldgen::ynv::Read(data, m, error))
				return Fail(name + ": " + error);
			const uint32_t own = m.index;
			m.index = remap(own);
			for (uint32_t& a : m.adjacent)
				a = remap(a);
			for (worldgen::ynv::Poly& p : m.polys)
			{
				if (p.Navmesh() != own)
					return Fail(std::format("{}: a polygon of navmesh {} in {}", name, p.Navmesh(), own));
				p.SetNavmesh(static_cast<uint16_t>(m.index));
			}
			for (auto& link : m.links)
			{
				uint32_t v;
				std::memcpy(&v, link.data() + 0x18, 4);
				const uint32_t from = remap(v & 0x3FFF), to = remap(v >> 14 & 0x3FFF);
				v = (v & 0xF0000000u) | from | to << 14;
				std::memcpy(link.data() + 0x18, &v, 4);
			}
			if (!worldgen::ynv::Write(m, written, error))
				return Fail(name + ": " + error);
			std::ofstream(out / name, std::ios::binary).write(reinterpret_cast<const char*>(written.data()), written.size());
			++count;
		}
		std::printf("%zu navmeshes renumbered for a grid %u wide\n", count, width);
		return 0;
	}
	if (command == "extract")
	{
		// game files by name (a '*' matches any run of characters), as stored; a resource also as its two blocks
		// (<name>.virtual, <name>.physical) for reading its structures
		if (!g_options.contains("out") || !g_options.contains("name"))
			return Fail("--out <dir> --name <pattern> are needed");
		const std::string pattern = g_options["name"];
		const auto match = [&](const std::string& name) {
			const size_t star = pattern.find('*');
			if (star == std::string::npos)
				return name == pattern;
			const std::string head = pattern.substr(0, star), tail = pattern.substr(star + 1);
			return name.size() >= head.size() + tail.size() && name.starts_with(head) && name.ends_with(tail);
		};
		const fs::path out = g_options["out"];
		fs::create_directories(out);
		size_t n = 0;
		for (const auto& [name, f] : files.Files())
		{
			if (!match(name))
				continue;
			Bytes data;
			if (!files.Read(f, data, error))
				return Fail(name + ": " + error);
			std::ofstream(out / name, std::ios::binary).write(reinterpret_cast<const char*>(data.data()), data.size());
			loader::convert::Resource r;
			if (loader::convert::Read(data, r, error))
			{
				std::ofstream(out / (name + ".virtual"), std::ios::binary).write(reinterpret_cast<const char*>(r.virtualBlock.data()), r.virtualBlock.size());
				std::ofstream(out / (name + ".physical"), std::ios::binary).write(reinterpret_cast<const char*>(r.physicalBlock.data()), r.physicalBlock.size());
			}
			++n;
		}
		std::printf("%zu files\n", n);
		return 0;
	}
	if (command == "testroad")
	{
		if (!g_options.contains("out") || !g_options.contains("path"))
			return Fail("--out <dir> --path x,y,z/x,y,z[/...] are needed");
		return TestRoad(files);
	}
	if (command == "regrid")
	{
		if (!g_options.contains("out"))
			return Fail("--out <dir> is needed");
		return RegridAll(files);
	}
	return Fail("unknown command " + command);
}
