#include "catalog.hpp"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <format>
#include <fstream>
#include <map>
#include <sstream>
#include <unordered_map>

#include "../ue5export/meta.hpp"

namespace worldgen::catalog
{
	namespace
	{
		using ue5export::meta::Hash;

		bool Has(const std::string& s, const char* part) { return s.find(part) != std::string::npos; }

		std::string AreaOf(const std::string& name)
		{
			const size_t a = name.find('_');
			if (a == std::string::npos)
				return {};
			const size_t b = name.find('_', a + 1);
			return name.substr(0, b == std::string::npos ? a : b);
		}
	}

	const char* Name(Kind kind)
	{
		switch (kind)
		{
		case Kind::Building: return "building";
		case Kind::Prop: return "prop";
		case Kind::Tree: return "tree";
		case Kind::Lod: return "lod";
		case Kind::Slod: return "slod";
		case Kind::Interior: return "interior";
		default: return "other";
		}
	}

	bool Build(const ue5export::GameFiles& files, std::vector<Item>& out, std::string& error)
	{
		// model names by hash (the archetypes only carry hashes)
		std::unordered_map<uint32_t, std::string> names;
		std::unordered_map<uint32_t, bool> fragments;
		for (const auto& [file, f] : files.Files())
		{
			const size_t dot = file.rfind('.');
			if (dot == std::string::npos)
				continue;
			const std::string ext = file.substr(dot), base = file.substr(0, dot);
			if (ext == ".ydr" || ext == ".yft" || ext == ".ydd")
			{
				const uint32_t h = loader::convert::Joaat(base);
				names.emplace(h, base);
				if (ext == ".yft")
					fragments[h] = true;
			}
		}
		// the archetypes
		std::unordered_map<uint32_t, size_t> index;
		for (const auto& [file, f] : files.Files())
		{
			if (!file.ends_with(".ytyp"))
				continue;
			loader::convert::Bytes data;
			ue5export::meta::Meta m;
			std::string ignored;
			if (!files.Read(f, data, ignored) || !m.Read(data, ignored))
				continue;
			for (const ue5export::meta::Ref& a : m.Root().Array(Hash("archetypes")))
			{
				if (!a)
					continue;
				const uint32_t h = a.Get<uint32_t>(Hash("name"));
				if (index.contains(h))
					continue;
				Item it;
				it.hash = h;
				if (const auto n = names.find(h); n != names.end())
					it.name = n->second;
				it.ytyp = file;
				it.txd = a.Get<uint32_t>(Hash("textureDictionary"));
				it.physics = a.Get<uint32_t>(Hash("physicsDictionary"));
				it.drawableDictionary = a.Get<uint32_t>(Hash("drawableDictionary"));
				it.flags = a.Get<uint32_t>(Hash("flags"));
				it.assetType = static_cast<uint8_t>(a.Get<uint32_t>(Hash("assetType")));
				it.fragment = fragments.contains(h);
				const auto lo = a.Get<std::array<float, 4>>(Hash("bbMin")), hi = a.Get<std::array<float, 4>>(Hash("bbMax"));
				it.min = {lo[0], lo[1], lo[2]};
				it.max = {hi[0], hi[1], hi[2]};
				it.lodDist = a.Get<float>(Hash("lodDist"));
				it.hdTextureDist = a.Get<float>(Hash("hdTextureDist"));
				it.area = AreaOf(it.name);
				if (a.type->name == Hash("CMloArchetypeDef"))
					it.kind = Kind::Interior;
				index.emplace(h, out.size());
				out.push_back(std::move(it));
			}
		}
		// the maps: how the game places each archetype, and the LOD parent of its HD entities
		struct Hd
		{
			uint32_t archetype, parentMap;
			int32_t parentIndex;
		};
		std::unordered_map<uint32_t, std::vector<uint32_t>> mapEntities; // map name -> its entities' archetypes
		std::vector<Hd> hds;
		std::unordered_map<uint32_t, std::array<uint32_t, 8>> levels;     // archetype -> entities per lod level
		std::unordered_map<uint32_t, std::vector<float>> distances;
		std::unordered_map<uint32_t, uint32_t> upright;
		for (const auto& [file, f] : files.Files())
		{
			if (!file.ends_with(".ymap"))
				continue;
			loader::convert::Bytes data;
			ue5export::meta::Meta m;
			std::string ignored;
			if (!files.Read(f, data, ignored) || !m.Read(data, ignored))
				continue;
			const ue5export::meta::Ref root = m.Root();
			const uint32_t name = root.Get<uint32_t>(Hash("name")), parent = root.Get<uint32_t>(Hash("parent"));
			auto& list = mapEntities[name];
			for (const ue5export::meta::Ref& e : root.Array(Hash("entities")))
			{
				if (!e)
				{
					list.push_back(0);
					continue;
				}
				const uint32_t archetype = e.Get<uint32_t>(Hash("archetypeName")), level = e.Get<uint32_t>(Hash("lodLevel"));
				list.push_back(archetype);
				++levels[archetype][std::min<uint32_t>(level, 7)];
				distances[archetype].push_back(e.Get<float>(Hash("lodDist")));
				const auto q = e.Get<std::array<float, 4>>(Hash("rotation"));
				if (std::fabs(q[0]) < 0.02f && std::fabs(q[1]) < 0.02f)
					++upright[archetype];
				const int32_t parentIndex = e.Get<int32_t>(Hash("parentIndex"));
				if ((level == 0 || level == 5) && parentIndex >= 0 && parent)
					hds.push_back({archetype, parent, parentIndex});
			}
		}
		std::unordered_map<uint32_t, std::map<uint32_t, int>> lodVotes;
		for (const Hd& h : hds)
			if (const auto it = mapEntities.find(h.parentMap); it != mapEntities.end() && h.parentIndex < static_cast<int32_t>(it->second.size()))
				if (const uint32_t lod = it->second[h.parentIndex])
					++lodVotes[h.archetype][lod];
		for (Item& it : out)
		{
			if (const auto l = levels.find(it.hash); l != levels.end())
			{
				const auto& n = l->second;
				it.uses = 0;
				for (const uint32_t c : n)
					it.uses += c;
				const int most = static_cast<int>(std::max_element(n.begin(), n.end()) - n.begin());
				if (it.kind != Kind::Interior)
				{
					if (most == 1)
						it.kind = Kind::Lod;
					else if (most == 2 || most == 3 || most == 4 || most == 6)
						it.kind = Kind::Slod;
				}
			}
			if (it.uses)
				it.upright = static_cast<float>(upright[it.hash]) / static_cast<float>(it.uses);
			if (auto d = distances.find(it.hash); d != distances.end() && !d->second.empty())
			{
				auto& v = d->second;
				std::nth_element(v.begin(), v.begin() + v.size() / 2, v.end());
				it.entityLodDist = v[v.size() / 2];
			}
			if (const auto v = lodVotes.find(it.hash); v != lodVotes.end())
			{
				it.lod = std::max_element(v->second.begin(), v->second.end(), [](const auto& a, const auto& b) { return a.second < b.second; })->first;
				if (const auto n = names.find(it.lod); n != names.end())
					it.lodName = n->second;
			}
			if (it.kind != Kind::Other)
				continue;
			const std::string& n = it.name;
			const float w = it.max[0] - it.min[0], d = it.max[1] - it.min[1], h = it.max[2] - it.min[2];
			if (Has(n, "tree") || Has(n, "palm") || Has(n, "bush") || Has(n, "shrub") || Has(n, "cypress") || Has(n, "_pine") ||
			    Has(n, "plant") || Has(n, "_veg_"))
				it.kind = Kind::Tree;
			else if (n.starts_with("prop_") || n.starts_with("p_") || it.fragment || it.physics)
				it.kind = Kind::Prop;
			else if (it.uses && it.assetType == 2 && h > 4.0f && w > 6.0f && d > 6.0f)
				it.kind = Kind::Building;
		}
		if (out.empty())
			return error = "no archetypes found", false;
		return true;
	}

	bool Save(const std::vector<Item>& items, const std::filesystem::path& file, std::string& error)
	{
		std::ofstream o(file, std::ios::binary);
		if (!o)
			return error = "cannot write " + file.string(), false;
		o << "name\thash\tkind\tarea\tytyp\ttxd\tphysics\tdrawableDictionary\tflags\tassetType\tfragment\tminX\tminY\tminZ\tmaxX\tmaxY\tmaxZ\tlodDist"
		     "\thdTextureDist\tuses\tlod\tlodName\tentityLodDist\tupright\n";
		for (const Item& it : items)
			o << std::format("{}\t{:08x}\t{}\t{}\t{}\t{:08x}\t{:08x}\t{:08x}\t{:x}\t{}\t{}\t{:.2f}\t{:.2f}\t{:.2f}\t{:.2f}\t{:.2f}\t{:.2f}\t{:.0f}\t{:.0f}\t{}\t{:08x}\t{}\t{:.0f}\t{:.2f}\n",
			    it.name, it.hash, Name(it.kind), it.area, it.ytyp, it.txd, it.physics, it.drawableDictionary, it.flags, it.assetType, it.fragment ? 1 : 0,
			    it.min[0], it.min[1], it.min[2], it.max[0], it.max[1], it.max[2], it.lodDist, it.hdTextureDist, it.uses, it.lod, it.lodName, it.entityLodDist,
			    it.upright);
		return true;
	}

	bool Load(const std::filesystem::path& file, std::vector<Item>& out, std::string& error)
	{
		std::ifstream in(file, std::ios::binary);
		if (!in)
			return error = "cannot read " + file.string() + " (worldgen catalog writes it)", false;
		std::string line;
		std::getline(in, line);
		const auto hex = [](const std::string& s) { return static_cast<uint32_t>(std::stoul(s, nullptr, 16)); };
		while (std::getline(in, line))
		{
			std::vector<std::string> f;
			std::istringstream words(line);
			for (std::string w; std::getline(words, w, '\t');)
				f.push_back(w);
			if (f.size() < 23)
				continue;
			Item it;
			it.name = f[0];
			it.hash = hex(f[1]);
			for (int k = 0; k <= static_cast<int>(Kind::Interior); ++k)
				if (f[2] == Name(static_cast<Kind>(k)))
					it.kind = static_cast<Kind>(k);
			it.area = f[3];
			it.ytyp = f[4];
			it.txd = hex(f[5]);
			it.physics = hex(f[6]);
			it.drawableDictionary = hex(f[7]);
			it.flags = hex(f[8]);
			it.assetType = static_cast<uint8_t>(std::stoi(f[9]));
			it.fragment = f[10] == "1";
			for (int a = 0; a < 3; ++a)
			{
				it.min[a] = std::stof(f[11 + a]);
				it.max[a] = std::stof(f[14 + a]);
			}
			it.lodDist = std::stof(f[17]);
			it.hdTextureDist = std::stof(f[18]);
			it.uses = static_cast<uint32_t>(std::stoul(f[19]));
			it.lod = hex(f[20]);
			it.lodName = f[21];
			it.entityLodDist = std::stof(f[22]);
			if (f.size() > 23)
				it.upright = std::stof(f[23]);
			out.push_back(std::move(it));
		}
		return true;
	}
}
