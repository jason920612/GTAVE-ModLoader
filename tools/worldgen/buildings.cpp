#include "buildings.hpp"

#include <algorithm>
#include <cmath>

namespace worldgen::buildings
{
	namespace
	{
		constexpr float kPi = 3.14159265f;

		bool Has(const std::string& s, const char* part) { return s.find(part) != std::string::npos; }

		// A model of a whole building (the game's names say so: build, house, apartments, tower...), not one of the
		// pieces round it (details, decals, glass, rails, ground, roads, cables...).
		bool IsBuilding(const catalog::Item& it)
		{
			static const char* kCity[] = {"dt1_", "bh1_", "vb_", "hw1_", "sc1_", "sm_", "ss1_", "kt1_", "bt1_", "sp1_", "ch1_", "ch2_", "ch3_", "id1_", "id2_", "po1_"};
			if (std::none_of(std::begin(kCity), std::end(kCity), [&](const char* p) { return it.name.starts_with(p); }))
				return false;
			static const char* kYes[] = {"build", "bld", "house", "hse", "apt", "villa", "motel", "hotel", "office", "tower", "shop", "store", "flats",
			    "warehouse", "hangar", "church", "mall", "condo", "garage", "block"};
			static const char* kNo[] = {"det", "decal", "dcl", "emissive", "emis", "glass", "glss", "rail", "pipe", "ovly", "lod", "intref", "_int",
			    "scaffold", "ground", "gnd", "wire", "cable", "ivy", "sign", "light", "dmg", "damage", "repair", "fizz", "alpha", "proxy", "shadow",
			    "_ref", "reflect", "frame", "_fr", "window", "door", "awning", "stair", "fence", "wall", "_rd", "fur", "grass", "_d_", "dirt", "blend",
			    "ladder", "_lad", "pole", "tarp", "struct", "_top", "fake", "_ob", "neon", "_em", "scaff", "billboard", "_bb", "antenna", "balcon",
			    "balus", "fnc", "dtl", "_dec", "shad", "shdw", "steps", "support", "fire_exit", "_fg", "sign", "burnt", "destroy", "_dam",
			    // (towers that are no buildings: water towers - Tallahassee had 163 of them on small footprints -, radio masts,
			    // a tower's pillars or base)
			    "wattower", "watertower", "radio", "pilar", "pillar", "towerbase", "tower_base", "tank", "silo", "chimney", "mast", "crane",
			    "sc1_33_tower", "pergola", "posts", "terrain", "_roof", "logo", "_fiz", "toilet"};
			if (std::none_of(std::begin(kYes), std::end(kYes), [&](const char* w) { return Has(it.name, w); }))
				return false;
			if (std::any_of(std::begin(kNo), std::end(kNo), [&](const char* w) { return Has(it.name, w); }) || it.name.ends_with("_d"))
				return false;
			const float w = it.max[0] - it.min[0], d = it.max[1] - it.min[1], h = it.max[2] - it.min[2];
			return w >= 6 && d >= 6 && h >= 4 && w <= 250 && d <= 250;
		}

		uint32_t Mix(uint32_t h)
		{
			h ^= h >> 16;
			h *= 0x7feb352d;
			h ^= h >> 15;
			h *= 0x846ca68b;
			return h ^ (h >> 16);
		}
	}

	void OrientedBox(const std::vector<P2>& f, P2& centre, float& angle, float& longSide, float& shortSide)
	{
		// the narrowest rectangle: one of its sides lies along an edge of the outline
		float bestArea = 1e30f;
		for (size_t k = 0; k < f.size(); ++k)
		{
			const P2& a = f[k];
			const P2& b = f[(k + 1) % f.size()];
			const float len = std::hypot(b[0] - a[0], b[1] - a[1]);
			if (len < 0.5f)
				continue;
			const float ux = (b[0] - a[0]) / len, uy = (b[1] - a[1]) / len;
			float u0 = 1e30f, u1 = -1e30f, v0 = 1e30f, v1 = -1e30f;
			for (const P2& p : f)
			{
				const float u = p[0] * ux + p[1] * uy, v = -p[0] * uy + p[1] * ux;
				u0 = std::min(u0, u), u1 = std::max(u1, u), v0 = std::min(v0, v), v1 = std::max(v1, v);
			}
			const float area = (u1 - u0) * (v1 - v0);
			if (area >= bestArea)
				continue;
			bestArea = area;
			const float cu = (u0 + u1) / 2, cv = (v0 + v1) / 2;
			centre = {cu * ux - cv * uy, cu * uy + cv * ux};
			if (u1 - u0 >= v1 - v0)
				angle = std::atan2(uy, ux), longSide = u1 - u0, shortSide = v1 - v0;
			else
				angle = std::atan2(ux, -uy), longSide = v1 - v0, shortSide = u1 - u0;
		}
	}

	Matcher::Matcher(const std::vector<catalog::Item>& catalog)
	{
		const auto bit = [](Style s) { return static_cast<uint8_t>(1u << static_cast<int>(s)); };
		for (const auto& it : catalog)
		{
			// (a whole building has a LOD of its own in the game's maps; the pieces round it - details, fences, balconies,
			// decals - do not)
			if (it.kind != catalog::Kind::Building || it.drawableDictionary || it.fragment || it.assetType != 2 || it.name.empty() || !it.uses ||
			    it.upright < 0.95f || !it.lod || !IsBuilding(it))
				continue;
			const float w = it.max[0] - it.min[0], d = it.max[1] - it.min[1], h = it.max[2] - it.min[2];
			const std::string& n = it.name;
			uint8_t styles = 0;
			if (n.starts_with("id1_") || n.starts_with("id2_") || n.starts_with("po1_"))
				styles = bit(Style::Industrial);
			else
			{
				const bool hills = n.starts_with("ch1_") || n.starts_with("ch2_") || n.starts_with("ch3_") || n.starts_with("bh1_");
				const bool beach = n.starts_with("vb_") || n.starts_with("sm_");
				if ((hills || beach) && h < 14)
					styles |= bit(Style::House);
				if (beach || n.starts_with("hw1_") || n.starts_with("kt1_") || n.starts_with("bh1_") || n.starts_with("sp1_"))
					styles |= bit(Style::Apartments);
				if (!hills || h >= 14)
					styles |= bit(Style::City);
			}
			candidates_.push_back({&it, std::max(w, d), std::min(w, d), h, w >= d, styles});
		}
		used_.assign(candidates_.size(), 0);
	}

	bool Matcher::Pick(const std::vector<P2>& footprint, float height, float ground, const std::string& kind, Placement& out)
	{
		if (footprint.size() < 3 || candidates_.empty())
			return false;
		P2 centre{};
		float angle = 0, longSide = 0, shortSide = 0;
		OrientedBox(footprint, centre, angle, longSide, shortSide);
		if (longSide < 5 || shortSide < 4)
			return false;
		// (most outlines only say "yes": small and low ones are houses, middling ones apartments)
		const float area = longSide * shortSide;
		const bool unknown = kind.empty() || kind == "yes";
		const Style style = kind == "house" || kind == "detached" || kind == "semidetached_house" || kind == "bungalow" || kind == "terrace" ||
		                            kind == "residential" || kind == "garage" || kind == "shed" || (unknown && area < 260 && height <= 9)
		                        ? (height > 12 ? Style::Apartments : Style::House)
		                    : kind == "apartments" || kind == "dormitory" || (unknown && area < 1200 && height <= 18) ? Style::Apartments
		                    : kind == "industrial" || kind == "warehouse" || kind == "manufacture" || kind == "hangar" ? Style::Industrial
		                                                                                                              : Style::City;
		const uint8_t want = static_cast<uint8_t>(1u << static_cast<int>(style));
		// the closest few: smaller is fine, bigger hardly (it would reach into the street), height matters less
		struct Scored
		{
			float cost;
			size_t c;
		};
		std::vector<Scored> best;
		for (size_t ci = 0; ci < candidates_.size(); ++ci)
		{
			const Candidate& c = candidates_[ci];
			if (!(c.styles & want))
				continue;
			const float rl = c.longSide / longSide, rs = c.shortSide / shortSide, rh = c.height / std::max(height, 3.0f);
			if (rl > 1.15f || rs > 1.2f || rl < 0.6f || rs < 0.55f || rh > 2.5f || rh < 0.3f)
				continue;
			const float cost = std::fabs(std::log(rl)) + std::fabs(std::log(rs)) + 0.6f * std::fabs(std::log(rh));
			if (cost > 1.3f)
				continue;
			best.push_back({cost, ci});
		}
		if (best.empty())
			return false;
		// among the closest dozen: the fit, how often it is used already, and a little chance (by the footprint's place)
		const uint32_t seed = Mix(static_cast<uint32_t>(std::lround(centre[0] * 7)) * 73856093u ^ static_cast<uint32_t>(std::lround(centre[1] * 7)) * 19349663u);
		const size_t keep = std::min<size_t>(best.size(), 12);
		std::partial_sort(best.begin(), best.begin() + static_cast<ptrdiff_t>(keep), best.end(), [](const Scored& a, const Scored& b) { return a.cost < b.cost; });
		size_t pick = 0;
		float pickScore = 1e30f;
		for (size_t k = 0; k < keep; ++k)
		{
			const float score = best[k].cost + 0.015f * static_cast<float>(used_[best[k].c]) + 0.15f * static_cast<float>(Mix(seed + static_cast<uint32_t>(k)) % 1000) / 1000.0f;
			if (score < pickScore)
				pickScore = score, pick = best[k].c;
		}
		++used_[pick];
		const Candidate& c = candidates_[pick];
		const catalog::Item& it = *c.item;
		// the model's long side along the footprint's (turned half round now and then)
		float heading = c.longIsX ? angle : angle - kPi / 2;
		if (seed & 0x100)
			heading += kPi;
		// the model's box centre over the footprint's, its bottom on the ground (a little sunk)
		const float lx = (it.min[0] + it.max[0]) / 2, ly = (it.min[1] + it.max[1]) / 2;
		const float ch = std::cos(heading), sh = std::sin(heading);
		out.item = &it;
		out.heading = heading;
		out.centre = centre;
		out.position = {centre[0] - (lx * ch - ly * sh), centre[1] - (lx * sh + ly * ch), ground - it.min[2] - 0.3f};
		// (no LOD models of their own here yet: drawn farther than the game draws them, the taller the farther)
		out.lodDist = std::clamp((it.max[2] - it.min[2]) * 30.0f, std::max(1000.0f, it.entityLodDist), 3000.0f);
		return true;
	}

	void CollisionBox(const Placement& p, std::vector<P3>& vertices, std::vector<std::array<uint32_t, 3>>& triangles)
	{
		const catalog::Item& it = *p.item;
		const float ch = std::cos(p.heading), sh = std::sin(p.heading);
		const uint32_t base = static_cast<uint32_t>(vertices.size());
		for (int k = 0; k < 8; ++k)
		{
			const float x = k & 1 ? it.max[0] : it.min[0], y = k & 2 ? it.max[1] : it.min[1], z = k & 4 ? it.max[2] : it.min[2];
			vertices.push_back({p.position[0] + x * ch - y * sh, p.position[1] + x * sh + y * ch, p.position[2] + z});
		}
		// faces outward (anticlockwise seen from outside): -z, +z, -y, +y, -x, +x
		static const uint32_t faces[6][4] = {{0, 2, 3, 1}, {4, 5, 7, 6}, {0, 1, 5, 4}, {2, 6, 7, 3}, {0, 4, 6, 2}, {1, 3, 7, 5}};
		for (const auto& f : faces)
		{
			triangles.push_back({base + f[0], base + f[1], base + f[2]});
			triangles.push_back({base + f[0], base + f[2], base + f[3]});
		}
	}
}
