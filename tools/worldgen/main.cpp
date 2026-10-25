// worldgen: the world builder's compiler (research/phase0.md §61). From the player's own game files.
//
//   worldgen roundtrip --game <dir> --keys <ng.bin>
//       every road-network region the game uses read, written back with our writer and read again: must be the same
//   worldgen regrid --game <dir> --keys <ng.bin> --out <dir>
//       the road network in 1024 m regions from -16384 (still 32 x 32; the loader's worldPathGrid): every node goes to
//       the region its position falls in, regions sorted (vehicle nodes, then pedestrian nodes, each by y) and renumbered,
//       links and junction refs pointed at the new numbers, junctions (and their height maps) moved with the nodes that
//       use them. Cayo Perico's set (1024 + N) on its own. Writes nodes<N>.ynd for every region with nodes and an empty
//       one for every old name that has none now (so the game's file of that name is not read into the new region).
//       Positions stay as they are: these regions lie within +-8192 m, where a region's origin is 0.
#include <Windows.h>

#include <algorithm>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <map>
#include <set>
#include <string>
#include <vector>

#include "../ue5export/gamefiles.hpp"
#include "../ue5export/ng.hpp"
#include "ynd.hpp"

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
		return Fail("usage: worldgen roundtrip|regrid --game <dir> --keys <ng.bin> [--out <dir>]");
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
	if (command == "regrid")
	{
		if (!g_options.contains("out"))
			return Fail("--out <dir> is needed");
		return RegridAll(files);
	}
	return Fail("unknown command " + command);
}
