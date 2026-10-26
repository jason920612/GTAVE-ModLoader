#include "paths.hpp"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <format>
#include <fstream>
#include <map>
#include <set>
#include <unordered_map>

#include "ynd.hpp"

namespace worldgen::paths
{
	namespace
	{
		using namespace worldgen::ynd;

		struct Out
		{
			uint32_t to;
			uint8_t flags[4];
		};

		struct GNode
		{
			double x = 0, y = 0;
			bool vehicle = true, dropped = false;
			Node raw{};
			std::vector<Out> links;
			// a game node's place in its file (for its junction refs)
			int gameArea = -1, gameIndex = -1;
			float junction = 0; // a new junction's record: its box's side (m)
			uint32_t area = 0, index = 0; // where it is written
		};

		void Origin(uint32_t area, int& x, int& y)
		{
			const int cx = static_cast<int>(area % 32), cy = static_cast<int>(area / 32);
			const bool inside = cx >= 8 && cx < 24 && cy >= 8 && cy < 24;
			x = inside ? 0 : (cx * 1024 - 16384) * 4;
			y = inside ? 0 : (cy * 1024 - 16384) * 4;
		}
	}

	bool Write(const ue5export::GameFiles& files, const std::array<float, 4>& cut, const std::vector<roads::PathNode>& nodes,
	    const std::vector<roads::PathLink>& links, const std::filesystem::path& out, Stats& stats, std::string& error)
	{
		std::map<uint32_t, Region> game;
		for (uint32_t n = 0; n < 1024; ++n)
		{
			const auto* f = files.Find("nodes" + std::to_string(n) + ".ynd");
			if (!f)
				continue;
			loader::convert::Bytes data;
			Region r;
			if (!files.Read(*f, data, error) || !Read(data, r, error))
				return error = f->path + ": " + error, false;
			game.emplace(n, std::move(r));
		}
		// the template: a plain two-way street node with two alike links (as tools/worldgen testroad takes it), one the GPS
		// routes over - +0x20 bit 1 set and +0x24 bit 0 clear, as the city's streets have them: the GPS's search for the
		// node to start from leaves the others out (research/phase0.md §61.26) - and not switched off
		const Node* tn = nullptr;
		Link ahead{};
		if (const auto it = game.find(463); it != game.end())
			for (uint32_t i = 0; i < it->second.vehicleNodes && !tn; ++i)
			{
				const Node& n = it->second.nodes[i];
				if (n.LinkCount() != 2 || !(n.raw[0x20] & 0x02) || (n.raw[0x24] & 0x91))
					continue;
				const Link& l0 = it->second.links[n.FirstLink()];
				const Link& l1 = it->second.links[n.FirstLink() + 1];
				if (std::memcmp(l0.flags, l1.flags, 3) == 0 && l0.flags[2] == 0x24)
					tn = &n, ahead = l1;
			}
		if (!tn)
			return error = "no template node in nodes463.ynd", false;

		std::vector<GNode> all;
		std::map<uint64_t, uint32_t> gameId; // (area << 32 | index) -> global
		for (const auto& [area, r] : game)
			for (uint32_t i = 0; i < r.nodes.size(); ++i)
			{
				GNode g;
				g.raw = r.nodes[i];
				g.x = r.nodes[i].X() / 4.0, g.y = r.nodes[i].Y() / 4.0;
				g.vehicle = i < r.vehicleNodes;
				g.gameArea = static_cast<int>(area), g.gameIndex = static_cast<int>(i);
				g.dropped = g.x >= cut[0] && g.x <= cut[2] && g.y >= cut[1] && g.y <= cut[3];
				gameId[static_cast<uint64_t>(area) << 32 | i] = static_cast<uint32_t>(all.size());
				all.push_back(std::move(g));
			}
		for (const auto& [area, r] : game)
			for (uint32_t i = 0; i < r.nodes.size(); ++i)
			{
				GNode& g = all[gameId[static_cast<uint64_t>(area) << 32 | i]];
				for (uint32_t k = 0; k < r.nodes[i].LinkCount(); ++k)
				{
					const Link& l = r.links[r.nodes[i].FirstLink() + k];
					const auto to = gameId.find(static_cast<uint64_t>(l.area) << 32 | l.node);
					if (to == gameId.end())
						continue;
					Out o{to->second, {}};
					std::memcpy(o.flags, l.flags, 4);
					g.links.push_back(o);
				}
			}
		for (GNode& g : all)
		{
			if (g.dropped)
				++stats.dropped;
			std::erase_if(g.links, [&](const Out& o) { return all[o.to].dropped; });
		}
		stats.gameNodes = all.size() - stats.dropped;
		// the new nodes
		const uint32_t first = static_cast<uint32_t>(all.size());
		for (const auto& n : nodes)
		{
			GNode g;
			g.raw = *tn;
			g.x = n.p[0], g.y = n.p[1];
			std::memset(g.raw.raw + 0x14, 0, 4); // (no street name)
			g.raw.raw[0x24] &= 0x7F;           // switched on
			// a junction (+0x24 bit 2) and a traffic light's stopping place (+0x21 bits 3-7: 15), as the game's own nodes
			// have them (research/phase0.md §61.27)
			g.raw.raw[0x24] = static_cast<uint8_t>(n.junction ? g.raw.raw[0x24] | 0x04 : g.raw.raw[0x24] & ~0x04);
			g.raw.raw[0x21] = static_cast<uint8_t>((g.raw.raw[0x21] & 0x07) | (n.light ? 15 << 3 : 0));
			if (n.junction)
				g.junction = n.junctionSize;
			const int16_t z = static_cast<int16_t>(std::lround(n.p[2] * 32));
			std::memcpy(g.raw.raw + 0x22, &z, 2);
			all.push_back(std::move(g));
		}
		stats.newNodes = nodes.size();
		for (const auto& l : links)
		{
			if (l.a < 0 || l.b < 0 || l.a == l.b)
				continue;
			const uint32_t a = first + static_cast<uint32_t>(l.a), b = first + static_cast<uint32_t>(l.b);
			const float length = static_cast<float>(std::hypot(all[a].x - all[b].x, all[a].y - all[b].y));
			for (const auto& [from, to, along, against] : {std::tuple{a, b, l.lanesAB, l.lanesBA}, std::tuple{b, a, l.lanesBA, l.lanesAB}})
			{
				Out o{to, {}};
				std::memcpy(o.flags, ahead.flags, 4);
				o.flags[2] = static_cast<uint8_t>((o.flags[2] & 3) | std::clamp(along, 0, 7) << 5 | std::clamp(against, 0, 7) << 2);
				// the second byte: bits 4-7 the gap between the directions (the template's 6 pushed the cars out over the
				// kerbs), bit 1 a narrow road - the AI's lanes 4.0 m wide instead of 5.4 (0x263AC00), as real roads' lanes
				// (3-3.7 m) are nearer to (research/phase0.md §61.27)
				const float lane = (l.width - 0.6f) / static_cast<float>(std::max(1, l.lanesAB + l.lanesBA));
				o.flags[1] = lane < 4.7f ? 0x02 : 0x00;
				o.flags[3] = static_cast<uint8_t>(std::min(255.0f, std::round(length)));
				all[from].links.push_back(o);
			}
			++stats.links;
		}
		// regions: every node's, ordered (vehicle nodes first, by y)
		std::map<uint32_t, std::vector<uint32_t>> regions;
		for (uint32_t i = 0; i < all.size(); ++i)
		{
			if (all[i].dropped)
				continue;
			const int cx = static_cast<int>(std::floor((all[i].x + 16384) / 1024)), cy = static_cast<int>(std::floor((all[i].y + 16384) / 1024));
			if (cx < 0 || cy < 0 || cx >= 32 || cy >= 32)
				return error = std::format("a node at {:.1f}, {:.1f} is outside the region grid", all[i].x, all[i].y), false;
			regions[static_cast<uint32_t>(cy * 32 + cx)].push_back(i);
		}
		for (auto& [area, list] : regions)
		{
			std::stable_sort(list.begin(), list.end(), [&](uint32_t a, uint32_t b) {
				if (all[a].vehicle != all[b].vehicle)
					return all[a].vehicle;
				return all[a].y < all[b].y;
			});
			if (list.size() > 0xFFFF)
				return error = std::format("region {}: {} nodes", area, list.size()), false;
			for (uint32_t k = 0; k < list.size(); ++k)
				all[list[k]].area = area, all[list[k]].index = k;
		}
		std::filesystem::create_directories(out);
		std::set<uint32_t> written;
		for (const auto& [area, list] : regions)
		{
			Region n;
			int ox, oy;
			Origin(area, ox, oy);
			std::map<uint64_t, uint16_t> junctionAt;
			for (uint32_t k = 0; k < list.size(); ++k)
			{
				const GNode& g = all[list[k]];
				Node node = g.raw;
				node.SetArea(static_cast<uint16_t>(area));
				node.SetIndex(static_cast<uint16_t>(k));
				node.SetFirstLink(static_cast<uint16_t>(n.links.size()));
				const int16_t x = static_cast<int16_t>(std::lround(g.x * 4) - ox), y = static_cast<int16_t>(std::lround(g.y * 4) - oy);
				std::memcpy(node.raw + 0x1C, &x, 2);
				std::memcpy(node.raw + 0x1E, &y, 2);
				for (const Out& o : g.links)
				{
					Link l{};
					l.area = static_cast<uint16_t>(all[o.to].area), l.node = static_cast<uint16_t>(all[o.to].index);
					std::memcpy(l.flags, o.flags, 4);
					n.links.push_back(l);
				}
				node.raw[0x25] = static_cast<uint8_t>((node.raw[0x25] & 7) | std::min<size_t>(g.links.size(), 31) << 3);
				if (g.vehicle)
					++n.vehicleNodes;
				n.nodes.push_back(node);
				// a new junction: a record of its own (the game's junctions with traffic lights all have one: research/
				// phase0.md §61.27) - its box (x, y its corner as the nodes' positions, 0.25 m), its heights (z in 1/32 m) and a
				// height map of 2 m cells over it, flat at the node's height
				if (g.gameArea < 0 && g.junction > 0)
				{
					const int cells = std::clamp(static_cast<int>(std::ceil(g.junction / 2)), 2, 120);
					Junction junction{};
					junction.x = static_cast<int16_t>(std::lround((g.x - cells) * 4) - ox);
					junction.y = static_cast<int16_t>(std::lround((g.y - cells) * 4) - oy);
					int16_t nz;
					std::memcpy(&nz, g.raw.raw + 0x22, 2);
					junction.minZ = junction.maxZ = nz;
					junction.width = junction.height = static_cast<uint8_t>(cells);
					junction.heightmap = static_cast<uint16_t>(n.heights.size());
					n.heights.insert(n.heights.end(), static_cast<size_t>(cells) * cells, 0);
					n.refs.push_back({static_cast<uint16_t>(area), static_cast<uint16_t>(k), static_cast<uint16_t>(n.junctions.size()), 0});
					// (+0x25 bit 0: the node has a junction record - the vehicle AI only makes a junction, with its traffic
					// lights, of a node on its route with this bit: 0xF4E880)
					n.nodes.back().raw[0x25] |= 0x01;
					n.junctions.push_back(junction);
				}
				// a game node's junction refs (and the junctions they name)
				if (g.gameArea < 0)
					continue;
				const Region& old = game.at(static_cast<uint32_t>(g.gameArea));
				for (const JunctionRef& ref : old.refs)
				{
					if (ref.area != g.gameArea || ref.node != g.gameIndex)
						continue;
					const uint64_t key = static_cast<uint64_t>(g.gameArea) << 32 | ref.junction;
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
					n.refs.push_back({static_cast<uint16_t>(area), static_cast<uint16_t>(k), j->second, ref.unused});
				}
			}
			std::sort(n.refs.begin(), n.refs.end(), [](const JunctionRef& a, const JunctionRef& b) { return a.node < b.node; });
			loader::convert::Bytes data;
			if (!ynd::Write(n, data, error))
				return error = std::format("region {}: {}", area, error), false;
			std::ofstream(out / std::format("nodes{}.ynd", area), std::ios::binary).write(reinterpret_cast<const char*>(data.data()), data.size());
			written.insert(area);
		}
		// the game's regions left with no nodes: empty ones over them
		Region empty;
		loader::convert::Bytes data;
		if (!ynd::Write(empty, data, error))
			return false;
		for (const auto& [area, r] : game)
			if (!written.contains(area))
			{
				std::ofstream(out / std::format("nodes{}.ynd", area), std::ios::binary).write(reinterpret_cast<const char*>(data.data()), data.size());
				written.insert(area);
			}
		stats.regions = written.size();
		return true;
	}
}
