#include "trees.hpp"

#include <algorithm>
#include <unordered_map>
#include <cmath>
#include <string_view>

#include "convert/resource.hpp"
#include "raster.hpp"

namespace worldgen::trees
{
	namespace
	{
		struct Model
		{
			const char* name;
			float height, width; // the model's (m)
		};

		// palms (Miami's: fan palms, royal and coconut palms - the tall ones with wide crowns)
		constexpr Model kPalms[] = {{"prop_palm_fan_02_a", 6.9f, 4.7f}, {"prop_palm_sm_01a", 7.7f, 6.2f}, {"prop_palm_fan_04_a", 9.1f, 4.0f},
		    {"prop_palm_fan_03_a", 11.0f, 4.7f}, {"prop_palm_fan_04_b", 12.2f, 5.6f}, {"prop_palm_sm_01d", 13.3f, 7.7f}, {"prop_palm_fan_03_b", 13.8f, 4.7f},
		    {"prop_palm_sm_01f", 15.9f, 8.5f}, {"prop_palm_fan_04_c", 19.5f, 5.7f}, {"prop_palm_sm_01e", 19.6f, 10.1f}, {"prop_palm_fan_03_c", 21.5f, 6.7f},
		    {"prop_palm_fan_04_d", 25.1f, 6.2f}, {"prop_palm_med_01a", 27.7f, 13.6f}, {"prop_palm_med_01b", 28.0f, 10.9f}};
		// broadleaf trees (figs first: the commonest shade trees there)
		// (no jacarandas: their purple flowers are rare there)
		constexpr Model kBroadleaf[] = {{"prop_tree_mquite_01", 5.2f, 5.6f}, {"prop_tree_maple_02", 8.2f, 9.8f}, {"prop_tree_lficus_03", 9.4f, 8.1f},
		    {"prop_tree_lficus_05", 14.7f, 15.2f},
		    {"prop_tree_lficus_06", 15.5f, 13.9f}, {"prop_tree_lficus_02", 16.6f, 19.4f}, {"prop_tree_olive_01", 17.2f, 17.8f},
		    {"prop_tree_eng_oak_01", 17.5f, 15.9f}, {"prop_tree_oak_01", 23.4f, 29.7f}};
		constexpr Model kBushes[] = {{"prop_bush_med_06", 2.0f, 3.1f}, {"prop_bush_med_03", 2.5f, 3.1f}, {"prop_bush_lrg_01", 2.6f, 6.1f},
		    {"prop_bush_lrg_01b", 3.2f, 5.7f}, {"prop_bush_lrg_03", 3.6f, 5.7f}};

		uint32_t Mix(uint32_t h)
		{
			h ^= h >> 16;
			h *= 0x7feb352du;
			h ^= h >> 15;
			h *= 0x846ca68bu;
			return h ^ (h >> 16);
		}

		// the model nearest in height (and crown, `crownWeight`), one of the three nearest by the place's own number
		template <size_t N>
		const Model& Choose(const Model (&models)[N], float height, float width, float crownWeight, uint32_t seed)
		{
			std::array<std::pair<float, size_t>, N> cost;
			for (size_t i = 0; i < N; ++i)
				cost[i] = {std::fabs(std::log(height / models[i].height)) + crownWeight * std::fabs(std::log(std::max(1.0f, width) / models[i].width)), i};
			std::sort(cost.begin(), cost.end());
			const size_t pick = seed % std::min<size_t>(3, N);
			// (a far worse fit is not worth the variety)
			return models[cost[pick].first < cost[0].first + 0.25f ? cost[pick].second : cost[0].second];
		}
	}

	namespace
	{
		// The scan's canopy in the box: `tree(x, y, height, crown radius, seed)` for each peak where a trunk may stand,
		// `bush(x, y, height, seed)` for low growth on a lattice of `bushStep` m (half of it when `halve`).
		template <typename Tree, typename Bush>
		void Peaks(elevation::Scan& scan, const std::function<bool(float, float)>& open, const std::function<float(float, float)>& ground,
		    const std::array<float, 4>& box, Tree&& tree, Bush&& bush, float bushStep = 6, bool halve = true)
		{
		constexpr float kPi = 3.14159265f;
		const float cell = scan.Cell();
		const float block = 256, margin = 16;
		for (float by = box[1]; by < box[3]; by += block)
			for (float bx = box[0]; bx < box[2]; bx += block)
			{
				const float x0 = bx - margin, y0 = by - margin;
				const int n = static_cast<int>((block + 2 * margin) / cell);
				// the canopy over the ground: vegetation returns only - or, in a scan that classes no vegetation at all in the
				// block (USGS's 2020 Panhandle scan: ground, buildings, water and bridges only), the returns 2 m and more over the
				// ground that are none of those (poles and wires thin to nothing in the smoothing below)
				bool classed = false;
				for (int j = 0; j < n && !classed; j += 4)
					for (int i = 0; i < n && !classed; i += 4)
						classed = (scan.Flags(x0 + (i + 0.5f) * cell, y0 + (j + 0.5f) * cell) & elevation::kVegetation) != 0;
				std::vector<float> v(static_cast<size_t>(n) * n, 0);
				bool any = false;
				for (int j = 0; j < n; ++j)
					for (int i = 0; i < n; ++i)
					{
						const float x = x0 + (i + 0.5f) * cell, y = y0 + (j + 0.5f) * cell;
						const uint8_t fl = scan.Flags(x, y);
						if (classed ? !(fl & elevation::kVegetation)
						            : (fl & (elevation::kBuilding | elevation::kWater | elevation::kBridge)) || !scan.Count(x, y))
							continue;
						const float top = scan.Top(x, y);
						if (std::isnan(top))
							continue;
						float g = scan.Ground(x, y);
						if (std::isnan(g))
							g = ground(x, y);
						if (!classed && top - g < 2.0f)
							continue;
						v[static_cast<size_t>(j) * n + i] = std::max(0.0f, top - g);
						any = true;
					}
				if (!any)
					continue;
				// smoothed (two 3 x 3 means): one peak a crown
				for (int pass = 0; pass < 2; ++pass)
				{
					std::vector<float> s(v.size(), 0);
					for (int j = 1; j + 1 < n; ++j)
						for (int i = 1; i + 1 < n; ++i)
						{
							float t = 0;
							for (int dj = -1; dj <= 1; ++dj)
								for (int di = -1; di <= 1; ++di)
									t += v[static_cast<size_t>(j + dj) * n + i + di];
							s[static_cast<size_t>(j) * n + i] = t / 9;
						}
					v = std::move(s);
				}
				const auto at = [&](int i, int j) { return i < 0 || j < 0 || i >= n || j >= n ? 0.0f : v[static_cast<size_t>(j) * n + i]; };
				const int inner0 = static_cast<int>(margin / cell), inner1 = n - inner0;
				const int step = std::max(1, static_cast<int>(std::lround(bushStep / cell)));
				for (int j = inner0; j < inner1; ++j)
					for (int i = inner0; i < inner1; ++i)
					{
						const float h = at(i, j);
						if (h < 1.2f)
							continue;
						const float x = x0 + (i + 0.5f) * cell, y = y0 + (j + 0.5f) * cell;
						if (x >= box[2] || y >= box[3])
							continue;
						const uint32_t seed = Mix(static_cast<uint32_t>(i + static_cast<int>(bx)) * 73856093u ^ static_cast<uint32_t>(j + static_cast<int>(by)) * 19349663u);
						if (h < 3.5f)
						{
							// low growth: a bush on a lattice, half of them (every entity counts against the game's pools)
							if (((i - inner0) % step) || ((j - inner0) % step) || (halve && (seed & 1)) || !open(x, y))
								continue;
							bush(x, y, h, seed);
							continue;
						}
						// a peak: the highest within a reach growing with the height
						const int reach = static_cast<int>(std::clamp(0.6f + 0.1f * h, 1.0f, 3.5f) / cell);
						bool peak = true;
						for (int dj = -reach; dj <= reach && peak; ++dj)
							for (int di = -reach; di <= reach && peak; ++di)
							{
								if ((di || dj) && di * di + dj * dj <= reach * reach)
								{
									const float o = at(i + di, j + dj);
									// (ties: the first in scan order wins)
									if (o > h || (o == h && (dj < 0 || (dj == 0 && di < 0))))
										peak = false;
								}
							}
						if (!peak || !open(x, y))
							continue;
						// the crown: out in 8 directions until under half the height or rising again (the next crown)
						float radius = 0;
						for (int d = 0; d < 8; ++d)
						{
							const float a = d * kPi / 4, ca = std::cos(a), sa = std::sin(a);
							float last = h, r = 0;
							for (float s = cell; s <= 12; s += cell)
							{
								const float o = at(i + static_cast<int>(std::lround(ca * s / cell)), j + static_cast<int>(std::lround(sa * s / cell)));
								if (o < 0.5f * h || o > last + 0.3f)
									break;
								last = std::min(last, o);
								r = s;
							}
							radius += r / 8;
						}
						tree(x, y, h, radius, seed);
					}
			}
		}
	}

	void Place(elevation::Scan& scan, const std::function<bool(float, float)>& open, const std::function<float(float, float)>& ground,
	    const std::array<float, 4>& box, std::vector<props::Prop>& out, Stats& stats)
	{
		constexpr float kPi = 3.14159265f;
		Peaks(
		    scan, open, ground, box,
		    [&](float x, float y, float h, float radius, uint32_t seed) {
			    const bool palm = radius < 0.33f * h;
			    const Model& m = palm ? Choose(kPalms, h, 2 * radius, 0.0f, seed) : Choose(kBroadleaf, h, 2 * radius, 0.7f, seed);
			    const float scale = std::clamp(h / m.height, palm ? 0.75f : 0.7f, palm ? 1.3f : 1.4f);
			    out.push_back({loader::convert::Joaat(m.name), {x, y, ground(x, y) - 0.2f}, static_cast<float>((seed >> 8) % 360) * kPi / 180,
			        palm ? 350.0f : 400.0f, scale});
			    ++(palm ? stats.palms : stats.broadleaf);
		    },
		    [&](float x, float y, float h, uint32_t seed) {
			    const Model& m = kBushes[(seed >> 1) % std::size(kBushes)];
			    out.push_back({loader::convert::Joaat(m.name), {x, y, ground(x, y) - 0.1f}, static_cast<float>((seed >> 8) % 360) * kPi / 180, 150,
			        std::clamp(h / m.height, 0.6f, 1.4f)});
			    ++stats.bushes;
		    });
	}

	void Undergrowth(const std::function<bool(float, float)>& where, const std::function<bool(float, float)>& open,
	    const std::function<float(float, float)>& ground, const std::array<float, 4>& box, std::vector<props::Prop>& out, Stats& stats)
	{
		constexpr float kPi = 3.14159265f, kStep = 6.0f;
		// (the big ones - lrg_01, 01b, 03: 2.6-3.6 m tall, 6 m across - and a smaller one between)
		constexpr Model kUnder[] = {{"prop_bush_lrg_01", 2.6f, 6.1f}, {"prop_bush_lrg_01b", 3.2f, 5.7f}, {"prop_bush_lrg_03", 3.6f, 5.7f},
		    {"prop_bush_med_03", 2.5f, 3.1f}};
		for (float y = std::floor(box[1] / kStep) * kStep; y < box[3]; y += kStep)
			for (float x = std::floor(box[0] / kStep) * kStep; x < box[2]; x += kStep)
			{
				const uint32_t seed = Mix(static_cast<uint32_t>(std::lround(x)) * 73856093u ^ static_cast<uint32_t>(std::lround(y)) * 19349663u ^ 0x5bd1e995u);
				if (seed % 10 >= 7)
					continue;
				const float px = x + ((seed >> 4) % 1000 / 1000.0f - 0.5f) * 4, py = y + ((seed >> 14) % 1000 / 1000.0f - 0.5f) * 4;
				if (px < box[0] || px >= box[2] || py < box[1] || py >= box[3] || !where(px, py) || !open(px, py))
					continue;
				const Model& m = kUnder[(seed >> 24) % std::size(kUnder)];
				out.push_back({loader::convert::Joaat(m.name), {px, py, ground(px, py) - 0.1f}, static_cast<float>((seed >> 8) % 360) * kPi / 180, 150,
				    0.8f + 0.6f * ((seed >> 20) % 16) / 15.0f});
				++stats.bushes;
			}
	}

	void LowGrowth(elevation::Scan& scan, const std::function<bool(float, float)>& where, const std::function<bool(float, float)>& open,
	    const std::function<float(float, float)>& ground, const std::array<float, 4>& box, std::vector<props::Prop>& out, Stats& stats)
	{
		constexpr float kPi = 3.14159265f;
		// (the medium ones, 3 m across: a hedge's run of them closes up)
		constexpr Model kHedge[] = {{"prop_bush_med_06", 2.0f, 3.1f}, {"prop_bush_med_03", 2.5f, 3.1f}};
		Peaks(
		    scan, open, ground, box, [](float, float, float, float, uint32_t) {},
		    [&](float x, float y, float h, uint32_t seed) {
			    if (!where(x, y))
				    return;
			    const Model& m = kHedge[(seed >> 1) % std::size(kHedge)];
			    out.push_back({loader::convert::Joaat(m.name), {x, y, ground(x, y) - 0.1f}, static_cast<float>((seed >> 8) % 360) * kPi / 180, 150,
			        std::clamp(h / m.height, 0.6f, 1.4f)});
			    ++stats.bushes;
		    },
		    3.0f, false);
	}

	std::vector<Seen> Detect(elevation::Scan& scan, const std::function<float(float, float)>& ground, const std::array<float, 4>& box)
	{
		std::vector<Seen> out;
		const std::function<bool(float, float)> anywhere = [](float, float) { return true; };
		Peaks(
		    scan, anywhere, ground, box, [&](float x, float y, float h, float radius, uint32_t) { out.push_back({x, y, h, radius, 0}); },
		    [](float, float, float, uint32_t) {});
		return out;
	}
}

namespace worldgen::trees
{
	namespace
	{
		struct Share
		{
			const char* name;
			int percent;
		};
		// (the game's: pines of Paleto's forests, its oaks, figs and fan palms - cabbage palms, Florida's state tree, and
		// for saw palmettos the small ones - its one cypress and its shrubs)
		constexpr Share kNorthPines[] = {{"prop_tree_pine_02", 50}, {"prop_tree_pine_01", 30}, {"prop_palm_fan_03_b", 10}, {"prop_tree_eng_oak_01", 10}};
		constexpr Share kSouthPines[] = {{"prop_tree_pine_02", 45}, {"prop_palm_fan_03_b", 20}, {"prop_palm_fan_03_a", 15}, {"prop_tree_oak_01", 10},
		    {"prop_tree_lficus_05", 10}};
		constexpr Share kNorthOaks[] = {{"prop_tree_oak_01", 40}, {"prop_tree_eng_oak_01", 40}, {"prop_tree_pine_02", 10}, {"prop_palm_fan_03_b", 10}};
		constexpr Share kSouthOaks[] = {{"prop_tree_oak_01", 30}, {"prop_tree_eng_oak_01", 25}, {"prop_tree_lficus_05", 15}, {"prop_tree_lficus_03", 10},
		    {"prop_palm_fan_03_b", 12}, {"prop_palm_fan_03_c", 8}};
		constexpr Share kMixed[] = {{"prop_tree_pine_02", 30}, {"prop_tree_oak_01", 25}, {"prop_tree_eng_oak_01", 25}, {"prop_palm_fan_03_b", 20}};
		constexpr Share kSwamp[] = {{"prop_tree_cypress_01", 55}, {"prop_palm_fan_03_a", 15}, {"prop_palm_fan_02_a", 10}, {"prop_bush_lrg_01", 12},
		    {"prop_bush_lrg_03", 8}};
		constexpr Share kScrub[] = {{"prop_bush_lrg_01", 30}, {"prop_bush_lrg_03", 30}, {"prop_bush_med_03", 20}, {"prop_palm_fan_02_a", 20}};
		constexpr Share kNorthYards[] = {{"prop_tree_oak_01", 30}, {"prop_tree_eng_oak_01", 25}, {"prop_tree_pine_02", 20}, {"prop_palm_fan_03_b", 15},
		    {"prop_palm_sm_01a", 10}};
		constexpr Share kSouthYards[] = {{"prop_tree_oak_01", 20}, {"prop_tree_lficus_05", 20}, {"prop_tree_lficus_03", 10}, {"prop_palm_fan_03_b", 20},
		    {"prop_palm_sm_01d", 15}, {"prop_palm_fan_03_c", 15}};

		template <size_t N>
		const char* Pick(const Share (&shares)[N], uint32_t roll)
		{
			int left = static_cast<int>(roll % 100);
			for (const Share& s : shares)
				if ((left -= s.percent) < 0)
					return s.name;
			return shares[0].name;
		}

		// A lattice point's tree by what covers the ground there (its NLCD class; 0 not known: the map's woods and
		// wetlands, 1 and 2 below): how likely a tree is there (out of 100) and of which kind.
		struct Kind
		{
			int chance;
			const char* (*pick)(bool south, uint32_t roll);
		};
		Kind KindOf(uint8_t cover)
		{
			switch (cover)
			{
			case 1: // (the map's woods)
			case 43:
				return {100, [](bool, uint32_t r) { return Pick(kMixed, r); }};
			case 42:
				return {100, [](bool south, uint32_t r) { return south ? Pick(kSouthPines, r) : Pick(kNorthPines, r); }};
			case 41:
				return {100, [](bool south, uint32_t r) { return south ? Pick(kSouthOaks, r) : Pick(kNorthOaks, r); }};
			case 2: // (the map's wetlands)
			case 90:
				return {70, [](bool, uint32_t r) { return Pick(kSwamp, r); }};
			case 52:
				return {35, [](bool, uint32_t r) { return Pick(kScrub, r); }};
			case 21:
				return {20, [](bool south, uint32_t r) { return south ? Pick(kSouthYards, r) : Pick(kNorthYards, r); }};
			case 22:
				return {15, [](bool south, uint32_t r) { return south ? Pick(kSouthYards, r) : Pick(kNorthYards, r); }};
			case 23:
				return {6, [](bool south, uint32_t r) { return south ? Pick(kSouthYards, r) : Pick(kNorthYards, r); }};
			case 24:
				return {2, [](bool south, uint32_t r) { return south ? Pick(kSouthYards, r) : Pick(kNorthYards, r); }};
			case 81:
				return {2, [](bool south, uint32_t r) { return south ? Pick(kSouthOaks, r) : Pick(kNorthOaks, r); }}; // (a pasture's lone oaks)
			default:
				return {0, nullptr};
			}
		}
	}

	void Fill(const std::vector<world::Area>& areas, const std::function<uint8_t(float, float)>& cover, double latitude,
	    const std::function<bool(float, float)>& open, const std::function<float(float, float)>& ground, const std::array<float, 4>& box,
	    std::vector<props::Prop>& out, Stats& stats)
	{
		constexpr float kPi = 3.14159265f, kSpacing = 22;
		// (the map's woods and wetlands, where the land cover is not known)
		raster::Grid<uint8_t> mapped;
		mapped.Init(box[0], box[1], box[2], box[3], kSpacing, 0);
		for (const auto& a : areas)
			if (a.kind == "wood" || a.kind == "wetland")
			{
				auto rings = a.outer;
				rings.insert(rings.end(), a.inner.begin(), a.inner.end());
				const uint8_t value = a.kind == "wood" ? 1 : 2;
				raster::ForPolygon(mapped, rings, [&](int i, int j) { mapped.At(i, j) = std::max(mapped.At(i, j), value); });
			}
		const bool south = latitude < 27;
		for (int j = 0; j < mapped.ny; ++j)
			for (int i = 0; i < mapped.nx; ++i)
			{
				const auto c = mapped.Centre(i, j);
				const uint32_t seed = Mix(static_cast<uint32_t>(std::lround(c[0])) * 73856093u ^ static_cast<uint32_t>(std::lround(c[1])) * 19349663u);
				const float x = c[0] + ((seed >> 8 & 255) / 255.0f - 0.5f) * 0.8f * kSpacing, y = c[1] + ((seed >> 16 & 255) / 255.0f - 0.5f) * 0.8f * kSpacing;
				if (x < box[0] || y < box[1] || x >= box[2] || y >= box[3])
					continue;
				const uint8_t known = cover ? cover(x, y) : 0;
				const Kind kind = KindOf(known ? known : mapped.At(i, j));
				if (!kind.pick || static_cast<int>(seed % 100) >= kind.chance || !open(x, y))
					continue;
				const uint32_t roll = Mix(seed);
				const char* name = kind.pick(south, roll);
				const bool palm = std::string_view(name).starts_with("prop_palm"), bush = std::string_view(name).starts_with("prop_bush");
				out.push_back({loader::convert::Joaat(name), {x, y, ground(x, y) - 0.2f}, static_cast<float>((roll >> 8) % 360) * kPi / 180,
				    bush ? 150.0f : 350.0f, 0.8f + static_cast<float>((roll >> 20) % 36) / 100});
				++(palm ? stats.palms : bush ? stats.bushes : stats.broadleaf);
			}
	}
}

namespace worldgen::trees
{
	namespace
	{
		// (the game's models of each kind, height and crown width as their boxes give them)
		// (pines of every age - the small conifers looked like spruces, the birches' white bark is not Florida's -, live
		// oaks, and the broadleaf trees that lose their leaves: maples and oaks)
		constexpr Model kPines[] = {{"prop_tree_pine_02", 29.5f, 16.5f}, {"prop_tree_pine_01", 31.4f, 15.0f}};
		// (the game has no crape myrtle - north Florida's small yard and street tree, in flower all summer -: its jacarandas)
		constexpr Model kFlowering[] = {{"prop_tree_jacada_02", 11.2f, 11.5f}, {"prop_tree_jacada_01", 14.3f, 14.5f}};
		constexpr Model kLiveOaks[] = {{"prop_tree_eng_oak_01", 17.5f, 17.2f}, {"prop_tree_oak_01", 23.4f, 29.7f}, {"prop_tree_olive_01", 17.2f, 18.9f}};
		constexpr Model kDeciduous[] = {{"prop_tree_maple_03", 9.1f, 5.9f}, {"prop_tree_maple_02", 8.2f, 10.0f}, {"prop_tree_eng_oak_01", 17.5f, 17.2f},
		    {"prop_tree_oak_01", 23.4f, 29.7f}};
		constexpr Model kCabbagePalms[] = {{"prop_palm_fan_03_a", 11.0f, 4.7f}, {"prop_palm_fan_03_b", 13.8f, 4.8f}, {"prop_palm_fan_03_c", 21.5f, 6.7f},
		    {"prop_palm_fan_04_b", 12.2f, 5.9f}};
	}

	void Photo(const std::vector<Seen>& seen, const std::function<uint8_t(float, float)>& cover, double latitude,
	    const std::function<bool(float, float)>& open, const std::function<float(float, float)>& ground, const std::array<float, 4>& box,
	    std::vector<props::Prop>& out, Stats& stats, const OwnTrees* own)
	{
		constexpr float kPi = 3.14159265f;
		const bool south = latitude < 27.5;
		// (one of our own of a kind: the nearest in height of the three nearest, by the tree's own number)
		const auto ownOf = [&](int kind, float h, uint32_t seed) -> const Own* {
			if (!own || (*own)[kind].empty())
				return nullptr;
			std::vector<const Own*> v;
			for (const Own& o : (*own)[kind])
				v.push_back(&o);
			std::sort(v.begin(), v.end(), [&](const Own* a, const Own* b) { return std::fabs(a->height - h) < std::fabs(b->height - h); });
			return v[seed % std::min<size_t>(3, v.size())];
		};
		// (the tallest first; one whose crown would lie mostly under a taller one's - closer than half their crowns' radii
		// together, at least 2.5 m - left out: a row of crape myrtles had come out a peak every 2-3 m, overlapping umbrellas)
		std::vector<const Seen*> order;
		for (const Seen& t : seen)
			order.push_back(&t);
		std::sort(order.begin(), order.end(), [](const Seen* a, const Seen* b) { return a->height > b->height; });
		std::unordered_map<int64_t, std::vector<std::array<float, 3>>> kept; // (by 10 m cell: x, y, crown)
		const auto cellOf = [](float x, float y) { return static_cast<int64_t>(std::floor(x / 10)) << 32 | static_cast<uint32_t>(static_cast<int32_t>(std::floor(y / 10))); };
		for (const Seen* tp : order)
		{
			const Seen& t = *tp;
			if (t.x < box[0] || t.y < box[1] || t.x >= box[2] || t.y >= box[3] || !open(t.x, t.y))
				continue;
			bool under = false;
			for (int dj = -1; dj <= 1 && !under; ++dj)
				for (int di = -1; di <= 1 && !under; ++di)
					if (const auto it = kept.find(cellOf(t.x + di * 10.0f, t.y + dj * 10.0f)); it != kept.end())
						for (const auto& k : it->second)
							if (std::hypot(k[0] - t.x, k[1] - t.y) < std::max(2.5f, 0.5f * (k[2] + t.crown)))
							{
								under = true;
								break;
							}
			if (under)
				continue;
			kept[cellOf(t.x, t.y)].push_back({t.x, t.y, t.crown});
			const uint32_t seed = Mix(static_cast<uint32_t>(std::lround(t.x * 4)) * 73856093u ^ static_cast<uint32_t>(std::lround(t.y * 4)) * 19349663u);
			// (the crown the canopy it covers in the pictures, a quarter wider: the game's models' leaves fill less than their
			// boxes, and the woods' canopy closes overhead as in the pictures)
			const float h = std::clamp(t.height, 3.0f, 40.0f), width = 2.5f * std::max(t.crown, 1.0f);
			const uint8_t nlcd = cover ? cover(t.x, t.y) : 0;
			const Model* m = nullptr;
			bool palm = false;
			// (cabbage palms: what the pictures take for one; in the south a quarter of the narrow evergreen crowns, in the
			// north's towns - Tallahassee's yards and medians - one in ten of them)
			const bool town = nlcd >= 21 && nlcd <= 24;
			if (t.kind == 2 || (t.kind == 0 && width < 0.45f * h && (south ? (seed & 3) == 0 : town && seed % 10 == 0)))
				m = &Choose(kCabbagePalms, h, width, 0.0f, seed), palm = true;
			else if (own)
			{
				// (ours: an evergreen a live oak unless it is a pine's height - over 21 m, or over 14 m and narrow -, a leafless
				// one a laurel or water oak, a small one in town a crape myrtle. The pictures' tree finder cuts a live oak's
				// crown into several peaks, each narrow for its height: by width alone they had all been pines - the reviewers'
				// rows of "lollipop" trees where the street pictures have a canopy of live oaks)
				// (and in town one in ten of the evergreens with a crown narrower than their height magnolias - three in ten had
				// taken the place of the live oaks the street pictures show -; a third of the leafless sweetgums)
				// (a town's small trees - under 8 m - its crape myrtles, whatever the pictures' winter says: shrunk live oaks had
				// stood in rows of small umbrellas where the street pictures have crape myrtles)
				const bool pine = h >= 21 || (h >= 14 && width < 0.4f * h);
				const bool magnolia = town && h >= 7 && h <= 16 && width < 0.8f * h && seed % 10 == 0;
				const int kind = town && h < 8 ? 3
				                 : t.kind == 0 ? (pine ? 1 : magnolia ? 4 : 0)
				                               : (town && h <= 9 && seed % 5 < 3 ? 3 : (seed >> 5) % 3 == 0 ? 5 : 2);
				if (const Own* o = ownOf(kind, h, seed))
				{
					props::Prop p;
					p.archetype = loader::convert::Joaat(o->name);
					p.position = {t.x, t.y, ground(t.x, t.y) - 0.15f};
					p.heading = static_cast<float>((seed >> 8) % 360) * kPi / 180;
					p.lodDist = 600;
					p.scaleZ = std::clamp(h / o->height, 0.4f, 1.6f);
					p.scale = std::clamp(std::clamp(width / o->width, 0.4f, 1.8f), p.scaleZ * 0.6f, p.scaleZ * 1.7f);
					out.push_back(p);
					++stats.broadleaf;
					continue;
				}
			}
			else if (t.kind == 0 && width >= 0.6f * h && h >= 8)
				m = &Choose(kLiveOaks, h, width, 0.7f, seed);
			else if (t.kind == 0)
				m = &Choose(kPines, h, width, 0.3f, seed);
			else if (town && h <= 9 && seed % 5 < 2)
				m = &Choose(kFlowering, h, width, 0.6f, seed); // (a town's small trees in flower: its crape myrtles)
			else
				m = &Choose(kDeciduous, h, width, nlcd == 90 ? 0.3f : 0.6f, seed);
			// (height and crown scaled apart, but not so far apart that a trunk turns into a pole or a barrel)
			const float sz = std::clamp(h / m->height, 0.25f, 1.4f);
			const float sxy = std::clamp(std::clamp(width / m->width, 0.3f, 1.5f), sz * 0.7f, sz * 1.8f);
			props::Prop p;
			p.archetype = loader::convert::Joaat(m->name);
			p.position = {t.x, t.y, ground(t.x, t.y) - 0.2f};
			p.heading = static_cast<float>((seed >> 8) % 360) * kPi / 180;
			p.lodDist = h < 11 ? 250.0f : 380.0f;
			p.scale = sxy;
			p.scaleZ = sz;
			out.push_back(p);
			++(palm ? stats.palms : stats.broadleaf);
		}
	}
}
