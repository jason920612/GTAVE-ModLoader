#include "trees.hpp"

#include <algorithm>
#include <cmath>

#include "convert/resource.hpp"

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

	void Place(elevation::Scan& scan, const std::function<bool(float, float)>& open, const std::function<float(float, float)>& ground,
	    const std::array<float, 4>& box, std::vector<props::Prop>& out, Stats& stats)
	{
		constexpr float kPi = 3.14159265f;
		const float cell = scan.Cell();
		const float block = 256, margin = 16;
		for (float by = box[1]; by < box[3]; by += block)
			for (float bx = box[0]; bx < box[2]; bx += block)
			{
				const float x0 = bx - margin, y0 = by - margin;
				const int n = static_cast<int>((block + 2 * margin) / cell);
				// the canopy over the ground: vegetation returns only
				std::vector<float> v(static_cast<size_t>(n) * n, 0);
				bool any = false;
				for (int j = 0; j < n; ++j)
					for (int i = 0; i < n; ++i)
					{
						const float x = x0 + (i + 0.5f) * cell, y = y0 + (j + 0.5f) * cell;
						if (!(scan.Flags(x, y) & elevation::kVegetation))
							continue;
						const float top = scan.Top(x, y);
						if (std::isnan(top))
							continue;
						float g = scan.Ground(x, y);
						if (std::isnan(g))
							g = ground(x, y);
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
							// low growth: a bush on a 6 m lattice, half of them (every entity counts against the game's pools)
							if (((i - inner0) % 12) || ((j - inner0) % 12) || (seed & 1) || !open(x, y))
								continue;
							const Model& m = kBushes[(seed >> 1) % std::size(kBushes)];
							out.push_back({loader::convert::Joaat(m.name), {x, y, ground(x, y) - 0.1f}, static_cast<float>((seed >> 8) % 360) * kPi / 180, 150,
							    std::clamp(h / m.height, 0.6f, 1.4f)});
							++stats.bushes;
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
						const bool palm = radius < 0.33f * h;
						const Model& m = palm ? Choose(kPalms, h, 2 * radius, 0.0f, seed) : Choose(kBroadleaf, h, 2 * radius, 0.7f, seed);
						const float scale = std::clamp(h / m.height, palm ? 0.75f : 0.7f, palm ? 1.3f : 1.4f);
						out.push_back({loader::convert::Joaat(m.name), {x, y, ground(x, y) - 0.2f}, static_cast<float>((seed >> 8) % 360) * kPi / 180,
						    palm ? 350.0f : 400.0f, scale});
						++(palm ? stats.palms : stats.broadleaf);
					}
			}
	}
}
