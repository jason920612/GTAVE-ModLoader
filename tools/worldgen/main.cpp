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
#include <map>
#include <memory>
#include <set>
#include <sstream>
#include <string>
#include <vector>

#include "../ue5export/drawable.hpp"
#include "convert/archive.hpp"
#include "convert/pso.hpp"
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

int main(int argc, char** argv)
{
	_setmaxstdio(8192);
	if (argc < 2)
		return Fail("usage: worldgen roundtrip|extract-ynd|regrid|testroad --game <dir> --keys <ng.bin> [--out <dir>]");
	for (int i = 2; i + 1 < argc; i += 2)
		if (std::string(argv[i]).starts_with("--"))
			g_options[argv[i] + 2] = argv[i + 1];
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
		// a DLC pack of the files in --in (maps, navmeshes, models...): ModLoader\mods\<name>\dlc.rpf. Its streaming archive is
		// a plain one (not an overlay): files the game does not have are registered as new (a map is then loaded where it is)
		if (!g_options.contains("in") || !g_options.contains("out") || !g_options.contains("name"))
			return Fail("--in <dir> --out <dlc.rpf> --name <pack name> are needed");
		const std::string name = g_options["name"], device = "dlc_" + name;
		loader::convert::WriteNode stream;
		stream.name = name + ".rpf";
		stream.kind = loader::convert::WriteNode::Kind::Archive;
		size_t count = 0;
		for (const auto& entry : fs::directory_iterator(g_options["in"]))
		{
			if (!entry.is_regular_file())
				continue;
			std::string file = entry.path().filename().string();
			std::transform(file.begin(), file.end(), file.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
			loader::convert::WriteNode n;
			n.name = file;
			std::ifstream in(entry.path(), std::ios::binary);
			n.data.assign(std::istreambuf_iterator<char>(in), {});
			n.kind = n.data.size() >= 4 && std::memcmp(n.data.data(), "RSC7", 4) == 0 ? loader::convert::WriteNode::Kind::Resource
			                                                                          : loader::convert::WriteNode::Kind::File;
			stream.children.push_back(std::move(n));
			++count;
		}
		// The archive is a map change set of story mode (GROUP_MAP_SP), as the game's own DLC maps are: the game then reads
		// the pack's map cache (x64\data\cacheloaderdata_dlc\<pack>_<change set hash>_cache_y.dat) for where each map and
		// collision file lies. Without it a new map is registered disabled and without its box, and never loaded.
		const std::string changeSet = name + "_MAP";
		// Archetype files (.ytyp): requested with the change set (DLC_ITYP_REQUEST), kept while it is on.
		std::string typeItems, typeEnables;
		for (const auto& child : stream.children)
			if (child.name.ends_with(".ytyp"))
			{
				const std::string path = std::format("{}:/%PLATFORM%/levels/gta5/{}.rpf/{}", device, name, child.name);
				typeItems += std::format("    <Item>\n      <filename>{}</filename>\n      <fileType>DLC_ITYP_REQUEST</fileType>\n"
				                         "      <overlay value=\"false\" />\n      <disabled value=\"true\" />\n      <persistent value=\"true\" />\n    </Item>\n",
				    path);
				typeEnables += std::format("            <Item>{}</Item>\n", path);
			}
		const std::string content = std::format(
		    "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n<CDataFileMgr__ContentsOfDataFileXml>\n  <disabledFiles />\n  <includedXmlFiles />\n"
		    "  <includedDataFiles />\n  <dataFiles>\n    <Item>\n      <filename>{0}:/%PLATFORM%/levels/gta5/{1}.rpf</filename>\n      <fileType>RPF_FILE</fileType>\n"
		    "      <overlay value=\"false\" />\n      <disabled value=\"true\" />\n      <persistent value=\"true\" />\n"
		    "      <contents>CONTENTS_DLC_MAP_DATA</contents>\n    </Item>\n{3}  </dataFiles>\n"
		    "  <contentChangeSets>\n    <Item>\n      <changeSetName>{2}</changeSetName>\n      <mapChangeSetData>\n        <Item>\n"
		    "          <associatedMap>MO_JIM_L11</associatedMap>\n          <filesToInvalidate />\n          <filesToEnable>\n"
		    "            <Item>{0}:/%PLATFORM%/levels/gta5/{1}.rpf</Item>\n{4}          </filesToEnable>\n        </Item>\n      </mapChangeSetData>\n"
		    "      <requiresLoadingScreen value=\"true\" />\n      <loadingScreenContext>LOADINGSCREEN_CONTEXT_LAST_FRAME</loadingScreenContext>\n"
		    "      <useCacheLoader value=\"true\" />\n    </Item>\n  </contentChangeSets>\n  <patchFiles />\n"
		    "</CDataFileMgr__ContentsOfDataFileXml>\n",
		    device, name, changeSet, typeItems, typeEnables);
		const std::string setup = std::format(
		    "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n<SSetupData>\n  <deviceName>{}</deviceName>\n  <datFile>content.xml</datFile>\n"
		    "  <timeStamp>00/00/0000 00:00:00</timeStamp>\n  <nameHash>{}</nameHash>\n  <contentChangeSets />\n  <contentChangeSetGroups>\n"
		    "    <Item>\n      <NameHash>GROUP_MAP_SP</NameHash>\n      <ContentChangeSets>\n        <Item>{}</Item>\n      </ContentChangeSets>\n"
		    "    </Item>\n  </contentChangeSetGroups>\n  <type>EXTRACONTENT_COMPAT_PACK</type>\n  <order value=\"999\" />\n</SSetupData>\n",
		    device, name, changeSet);
		// The cache (version 46): a header, <fileDates> (each archive: hash of its path, a file time), then per module its
		// entries after their byte count. fwMapDataStore, 64 bytes a map: name, parent, content flags, streaming box, entities
		// box, flags (1: streamed like the game's own maps). BoundsStore, 32 bytes a collision file: name, box, flags (0).
		Bytes cache(0x64, 0);
		const std::string head = "[VERSION]\n46\n";
		std::memcpy(cache.data(), head.data(), head.size());
		const auto append = [&](const std::string& s) { cache.insert(cache.end(), s.begin(), s.end()); };
		append("<fileDates>\n</fileDates>\n");
		Bytes maps, bounds;
		const auto put32 = [](Bytes& b, uint32_t v) { b.insert(b.end(), reinterpret_cast<uint8_t*>(&v), reinterpret_cast<uint8_t*>(&v) + 4); };
		const auto putf = [](Bytes& b, float v) { b.insert(b.end(), reinterpret_cast<uint8_t*>(&v), reinterpret_cast<uint8_t*>(&v) + 4); };
		for (const auto& child : stream.children)
		{
			const std::string base = child.name.substr(0, child.name.rfind('.'));
			if (child.name.ends_with(".ymap"))
			{
				ue5export::meta::Meta mm;
				if (!mm.Read(child.data, error))
					return Fail(child.name + ": " + error);
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
					return Fail(child.name + ": " + error);
				const ue5export::drawable::View view(res);
				put32(bounds, loader::convert::Joaat(base));
				for (const uint64_t at : {ue5export::drawable::View::kRoot + 0x30, ue5export::drawable::View::kRoot + 0x20})
					for (int a = 0; a < 3; ++a)
						putf(bounds, view.Get<float>(at + 4 * a));
				put32(bounds, 0);
			}
		}
		for (const auto& [module, payload] : {std::pair<const char*, const Bytes*>{"fwMapDataStore", &maps}, {"CInteriorProxy", nullptr}, {"BoundsStore", &bounds}})
		{
			append(std::format("<module>\n{}\n", module));
			put32(cache, payload ? static_cast<uint32_t>(payload->size()) : 0);
			if (payload)
				cache.insert(cache.end(), payload->begin(), payload->end());
			append("</module>\n");
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
		std::string lowerName = name;
		std::transform(lowerName.begin(), lowerName.end(), lowerName.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
		loader::convert::WriteNode data, loaderData, cacheFile;
		data.name = "data";
		loaderData.name = "cacheloaderdata_dlc";
		cacheFile.name = std::format("{}_{}_cache_y.dat", lowerName, loader::convert::Joaat(changeSet));
		cacheFile.kind = loader::convert::WriteNode::Kind::File;
		cacheFile.data = std::move(cache);
		loaderData.children.push_back(std::move(cacheFile));
		data.children.push_back(std::move(loaderData));
		platform.children.push_back(std::move(data));
		// (where the game's own map archives are: x64\levels\gta5)
		loader::convert::WriteNode levels, gta5;
		levels.name = "levels";
		gta5.name = "gta5";
		gta5.children.push_back(std::move(stream));
		levels.children.push_back(std::move(gta5));
		platform.name = "x64";
		platform.children.push_back(std::move(levels));
		root.children.push_back(std::move(platform));
		Bytes packed;
		if (!loader::convert::BuildArchive(root, packed, error))
			return Fail(error);
		fs::create_directories(fs::path(g_options["out"]).parent_path());
		std::ofstream(g_options["out"], std::ios::binary).write(reinterpret_cast<const char*>(packed.data()), packed.size());
		std::printf("%s: %zu files in %s.rpf (%zu bytes)\n", g_options["out"].c_str(), count, name.c_str(), packed.size());
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
		int shown = 0;
		for (const ue5export::meta::Ref& e : root.Array(Hash("entities")))
		{
			if (!e || shown++ >= 12)
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
