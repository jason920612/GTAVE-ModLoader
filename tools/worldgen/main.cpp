// worldgen: the world builder's compiler (research/phase0.md §61). From the player's own game files.
//
//   worldgen roundtrip --game <dir> --keys <ng.bin>
//       every road-network region the game uses read, written back with our writer and read again: must be the same
//   worldgen ynv-roundtrip --game <dir> --keys <ng.bin>
//       every navmesh the game uses read, written back with our writer and read again: must be the same
//   worldgen extract --game <dir> --keys <ng.bin> --out <dir> --name <pattern>
//       game files by name ("*" matches any run of characters), as stored; a resource also as its two blocks
//   worldgen extract-ynd --game <dir> --keys <ng.bin> --out <dir>
//       the road-network region files the game uses, as they are
//   worldgen testroad --game <dir> --keys <ng.bin> --out <dir> --path x,y,z/x,y,z[/...] [--spacing m] [--lanes n]
//       [--template x,y]
//       a two-way test road along the polyline for the 1024 m grid (a road node of the game copied: its flags, switched
//       on; n lanes each way), positions relative to the region's corner outside +-8192 m
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
#include <fstream>
#include <map>
#include <set>
#include <sstream>
#include <string>
#include <vector>

#include "../ue5export/gamefiles.hpp"
#include "../ue5export/ng.hpp"
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
