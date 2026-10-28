#include "props.hpp"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <string_view>
#include <unordered_map>
#include <vector>

#include "convert/resource.hpp"
#include "polygon.hpp"

namespace worldgen::props
{
	namespace
	{
		constexpr float kPi = 3.14159265f;

		uint32_t Mix(uint32_t h)
		{
			h ^= h >> 16;
			h *= 0x7feb352d;
			h ^= h >> 15;
			h *= 0x846ca68b;
			return h ^ (h >> 16);
		}
		uint32_t Seed(float x, float y) { return Mix(static_cast<uint32_t>(std::lround(x * 10)) * 73856093u ^ static_cast<uint32_t>(std::lround(y * 10)) * 19349663u); }

		bool In(const std::array<float, 4>& b, float x, float y) { return x >= b[0] && x < b[2] && y >= b[1] && y < b[3]; }

		// the point at distance s along a polyline, its direction and height
		bool At(const roads::Edge& e, float s, P2& p, P2& d, float& z)
		{
			float run = 0;
			for (size_t i = 0; i + 1 < e.points.size(); ++i)
			{
				const P2& a = e.points[i];
				const P2& b = e.points[i + 1];
				const float l = std::hypot(b[0] - a[0], b[1] - a[1]);
				if (run + l >= s && l > 0)
				{
					const float t = (s - run) / l;
					p = {a[0] + (b[0] - a[0]) * t, a[1] + (b[1] - a[1]) * t};
					d = {(b[0] - a[0]) / l, (b[1] - a[1]) / l};
					z = e.z[i] + (e.z[i + 1] - e.z[i]) * t;
					return true;
				}
				run += l;
			}
			return false;
		}

		// a road's rank at a junction: the bigger kinds go on, the smaller stop (service roads and driveways give way with no
		// sign of their own)
		int Rank(const roads::Edge& e)
		{
			const std::string_view k = e.kind;
			return k.starts_with("motorway") ? 7 : k.starts_with("trunk") ? 6 : k.starts_with("primary") ? 5 : k.starts_with("secondary") ? 4
			     : k.starts_with("tertiary")                                                                         ? 3
			     : k == "unclassified" || k == "residential" || k == "living_street" || k == "road"                ? 2
			                                                                                                         : 1;
		}

		float Length(const roads::Edge& e)
		{
			float l = 0;
			for (size_t i = 0; i + 1 < e.points.size(); ++i)
				l += std::hypot(e.points[i + 1][0] - e.points[i][0], e.points[i + 1][1] - e.points[i][1]);
			return l;
		}
	}

	void Place(const world::World& w, const roads::Network& n, const std::vector<roads::Signal>& signals, const std::function<bool(float, float)>& open,
	    const std::function<float(float, float)>& ground, const std::array<float, 4>& box, std::vector<Prop>& out, bool palms)
	{
		// palms of a few sizes (Miami's mix: mostly fan palms, some tall ones)
		static const char* kPalms[] = {"prop_palm_fan_03_a", "prop_palm_fan_03_b", "prop_palm_fan_04_a", "prop_palm_fan_04_b", "prop_palm_sm_01a",
		    "prop_palm_sm_01d", "prop_palm_sm_01e", "prop_palm_fan_03_c", "prop_palm_med_01a", "prop_palm_med_01b"};
		const auto addPalm = [&](float x, float y, float z) {
			const uint32_t h = Seed(x, y);
			out.push_back({loader::convert::Joaat(kPalms[h % std::size(kPalms)]), {x, y, z - 0.2f}, static_cast<float>((h >> 8) % 360) * kPi / 180, 300});
		};
		// the world's trees (on land, off the roads, outside buildings)
		for (const auto& p : w.points)
			if (palms && p.kind == "tree" && In(box, p.position[0], p.position[1]) && open(p.position[0], p.position[1]))
			{
				const float g = ground(p.position[0], p.position[1]);
				if (g > 0.5f)
					addPalm(p.position[0], p.position[1], g);
			}
		const uint32_t light = loader::convert::Joaat("prop_streetlight_01"), signal = loader::convert::Joaat("prop_traffic_01a");
		for (const auto& e : n.edges)
		{
			if (!e.sidewalks || e.bridge)
				continue;
			// (a street light of its own on the bigger roads - the residential streets' hang on the power poles, the street
			// pictures there have no steel poles)
			const bool lit = !(e.kind == "residential" || e.kind == "unclassified" || e.kind == "living_street" || e.kind == "service" ||
			                   e.kind == "road" || e.kind == "track");
			const float total = Length(e);
			const float side = e.HalfWidth() + 0.6f; // (on the sidewalk, near the kerb)
			// (none by a street junction's corner, where turning cars cut across the kerb: from past its reach)
			const float fromA = roads::CornerReach(n, e.a) + 1, fromB = roads::CornerReach(n, e.b) + 1;
			// street lights every 45 m on the right of the way (both sides on wide roads), their arm (the model's -y)
			// over the road
			for (float s = std::max(12.0f, fromA); lit && s < total - std::max(12.0f, fromB); s += 45)
			{
				P2 p, d;
				float z;
				if (!At(e, s, p, d, z))
					break;
				const P2 right{d[1], -d[0]};
				for (const float sign : {1.0f, -1.0f})
				{
					if (sign < 0 && e.forward + e.backward < 4)
						continue;
					const P2 q{p[0] + right[0] * side * sign, p[1] + right[1] * side * sign};
					if (!In(box, q[0], q[1]))
						continue;
					// the model's x axis along -d on the right (its -y then points left, at the road), along +d on the left
					const float heading = sign > 0 ? std::atan2(-d[1], -d[0]) : std::atan2(d[1], d[0]);
					out.push_back({light, {q[0], q[1], z + roads::kKerb}, heading, 250});
				}
			}
			// palms along the avenues, every 30 m on alternating sides, in the sidewalk's middle
			if (palms && (e.kind.starts_with("primary") || e.kind.starts_with("secondary") || e.kind.starts_with("trunk")))
				for (float s = std::max(20.0f, fromA); s < total - std::max(20.0f, fromB); s += 30)
				{
					P2 p, d;
					float z;
					if (!At(e, s, p, d, z))
						break;
					const float sign = static_cast<int>(s / 30) % 2 ? 1.0f : -1.0f;
					const P2 right{d[1], -d[0]};
					const float o = e.HalfWidth() + std::max(e.Walk(), 1.6f) * 0.65f;
					const P2 q{p[0] + right[0] * o * sign, p[1] + right[1] * o * sign};
					if (In(box, q[0], q[1]))
						addPalm(q[0], q[1], z + roads::kKerb + 0.2f);
				}
		}
		// stop signs at the junctions without signals (the street pictures' every corner): on the right of each road coming in
		// that stops - the smaller where a smaller meets a bigger, the stem of a T of equals, every way at a crossroads of
		// residential streets (an all-way stop) -, 1.5 m before the corner's reach, 0.8 m off the asphalt, facing the drivers
		const uint32_t stop = loader::convert::Joaat("prop_sign_road_01a");
		// (where the map has its stop signs - a town's mapped as Tallahassee's 813 are -, those; else by the rule below)
		if (n.mappedStops >= 20)
		{
			for (const roads::Edge& e : n.edges)
				for (const bool atA : {true, false})
				{
					if (!(atA ? e.stopA : e.stopB) || e.points.size() < 2 || e.bridge)
						continue;
					const roads::Vertex& v = n.vertices[static_cast<size_t>(atA ? e.a : e.b)];
					const P2 p0 = atA ? e.points[1] : e.points[e.points.size() - 2], p1 = atA ? e.points[0] : e.points.back();
					const float l = std::hypot(p1[0] - p0[0], p1[1] - p0[1]);
					if (l < 1e-3f)
						continue;
					const P2 in{(p1[0] - p0[0]) / l, (p1[1] - p0[1]) / l}, right{in[1], -in[0]};
					const float back = roads::CornerReach(n, atA ? e.a : e.b) + 1.5f, aside = e.HalfWidth() + 0.8f;
					const P2 q{v.p[0] - in[0] * back + right[0] * aside, v.p[1] - in[1] * back + right[1] * aside};
					if (In(box, q[0], q[1]) && open(q[0], q[1]))
						out.push_back({stop, {q[0], q[1], ground(q[0], q[1])}, std::atan2(-in[0], in[1]), 200});
				}
		}
		for (size_t vi = 0; vi < n.vertices.size() && n.mappedStops < 20; ++vi)
		{
			const auto& v = n.vertices[vi];
			if (v.signals || v.edges.size() < 3)
				continue;
			struct Way
			{
				const roads::Edge* e;
				P2 in; // (the way traffic comes in, towards the junction)
				int rank;
			};
			std::vector<Way> ways;
			for (const int ei : v.edges)
			{
				const roads::Edge& e = n.edges[static_cast<size_t>(ei)];
				if (e.points.size() < 2 || e.a == e.b)
					continue;
				const bool atA = e.a == static_cast<int>(vi);
				const P2 p = atA ? e.points[0] : e.points.back(), q = atA ? e.points[1] : e.points[e.points.size() - 2];
				const float l = std::hypot(p[0] - q[0], p[1] - q[1]);
				if (l < 1e-3f)
					continue;
				// (none comes in on a one-way road leaving the junction)
				if ((atA && e.backward == 0) || (!atA && e.forward == 0))
				{
					ways.push_back({&e, {(p[0] - q[0]) / l, (p[1] - q[1]) / l}, -1});
					continue;
				}
				ways.push_back({&e, {(p[0] - q[0]) / l, (p[1] - q[1]) / l}, Rank(e)});
			}
			int top = 0;
			for (const Way& wy : ways)
				top = std::max(top, wy.rank);
			if (top < 2)
				continue;
			const bool equals = std::all_of(ways.begin(), ways.end(), [&](const Way& wy) { return wy.rank == top || wy.rank < 0; });
			// (a T of equals: the two most nearly opposite go on, the other stops)
			std::vector<bool> stops(ways.size(), false);
			if (equals && ways.size() == 3)
			{
				float straightest = 1;
				size_t a = 0, b = 1;
				for (size_t i = 0; i < ways.size(); ++i)
					for (size_t j = i + 1; j < ways.size(); ++j)
						if (const float dot = ways[i].in[0] * ways[j].in[0] + ways[i].in[1] * ways[j].in[1]; dot < straightest)
							straightest = dot, a = i, b = j;
				for (size_t i = 0; i < ways.size(); ++i)
					stops[i] = i != a && i != b;
			}
			else
				for (size_t i = 0; i < ways.size(); ++i)
					stops[i] = equals ? top == 2 : ways[i].rank < top;
			const float reach = roads::CornerReach(n, static_cast<int>(vi));
			for (size_t i = 0; i < ways.size(); ++i)
			{
				const Way& wy = ways[i];
				if (!stops[i] || wy.rank < 2 || wy.e->bridge)
					continue;
				const P2 right{wy.in[1], -wy.in[0]};
				const float back = reach + 1.5f, aside = wy.e->HalfWidth() + 0.8f;
				const P2 q{v.p[0] - wy.in[0] * back + right[0] * aside, v.p[1] - wy.in[1] * back + right[1] * aside};
				if (!In(box, q[0], q[1]) || !open(q[0], q[1]))
					continue;
				// (the sign's face its model's -y: towards the traffic coming in)
				out.push_back({stop, {q[0], q[1], ground(q[0], q[1])}, std::atan2(-wy.in[0], wy.in[1]), 200});
			}
		}
		// traffic signal masts where the junctions' geometry put them (roads::Geometry): the long arm (9.8 m) where the lanes
		// coming in are wider than the short one (5 m) reaches
		const uint32_t longSignal = loader::convert::Joaat("prop_traffic_01b");
		for (const roads::Signal& s : signals)
			if (In(box, s.p[0], s.p[1]))
				out.push_back({s.reach > 5.5f ? longSignal : signal, s.p, s.heading, 300});
	}

	bool MemorialPark(const std::string& name)
	{
		std::string n = name;
		std::transform(n.begin(), n.end(), n.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
		for (const char* k : {"memorial park", "memory garden", "memorial garden", "garden of memor", "gardens of memor", "national cemetery", "columbarium"})
			if (n.find(k) != std::string::npos)
				return true;
		return false;
	}

	void Graves(const std::vector<world::Area>& areas, const std::vector<world::Walk>& walks, const std::function<bool(float, float)>& open,
	    const std::function<float(float, float)>& ground, const std::array<float, 4>& box, std::vector<Prop>& out, GraveStats& stats)
	{
		// the game's (Los Santos's cemetery's - the prologue's have snow on them): along the row the model's x, its face
		// its -y; its box's middle in its own axes; how many plots (1.9 m) along the row and rows (4 m) across it it takes
		struct Kind
		{
			const char* name;
			float width, depth;
			P2 centre;
			int plots, rows, weight;
		};
		// (upright headstones and monuments most, an obelisk now and then, few raised tombs: the reviewers saw "New Orleans
		// style tombs and crosses" where the street pictures have obelisks and upright stones)
		static const Kind kKinds[] = {{"prop_gravestones_01a", 5.0f, 0.36f, {-0.175f, 0.0f}, 3, 1, 14}, {"prop_gravestones_02a", 3.71f, 0.39f, {-0.185f, 0.0f}, 2, 1, 20},
		    {"prop_gravestones_04a", 3.4f, 3.16f, {0.0f, 0.0f}, 2, 1, 3}, {"prop_gravestones_05a", 2.11f, 0.57f, {0.0f, 1.585f}, 2, 1, 10},
		    {"prop_gravestones_07a", 2.26f, 0.82f, {-0.01f, 0.0f}, 2, 1, 14}, {"prop_gravestones_08a", 1.58f, 1.52f, {0.0f, 0.0f}, 1, 1, 6},
		    {"prop_gravestones_09a", 0.94f, 0.96f, {0.0f, 0.0f}, 1, 1, 12}, {"prop_gravetomb_01a", 1.88f, 3.66f, {0.0f, 0.0f}, 1, 1, 5},
		    {"prop_gravetomb_02a", 4.16f, 5.16f, {0.0f, 0.0f}, 3, 2, 1}};
		int total = 0;
		for (const Kind& k : kKinds)
			total += k.weight;
		constexpr float kPlot = 1.9f, kRow = 4.0f;
		// (things by 8 m cells: segments - the paths', the outline's - and props already there)
		using Key = int64_t;
		const auto keyOf = [](float x, float y) {
			return static_cast<Key>(std::floor(x / 8)) << 32 | static_cast<uint32_t>(static_cast<int32_t>(std::floor(y / 8)));
		};
		struct Segment
		{
			P2 a, b;
			float reach;
		};
		const auto addSegment = [&](std::unordered_map<Key, std::vector<Segment>>& grid, const Segment& s) {
			const float x0 = std::min(s.a[0], s.b[0]) - s.reach, x1 = std::max(s.a[0], s.b[0]) + s.reach;
			const float y0 = std::min(s.a[1], s.b[1]) - s.reach, y1 = std::max(s.a[1], s.b[1]) + s.reach;
			for (float y = std::floor(y0 / 8) * 8; y <= y1; y += 8)
				for (float x = std::floor(x0 / 8) * 8; x <= x1; x += 8)
					grid[keyOf(x + 1, y + 1)].push_back(s);
		};
		const auto distance = [](const P2& p, const P2& a, const P2& b) {
			const float dx = b[0] - a[0], dy = b[1] - a[1], l = dx * dx + dy * dy;
			const float t = l > 0 ? std::clamp(((p[0] - a[0]) * dx + (p[1] - a[1]) * dy) / l, 0.0f, 1.0f) : 0.0f;
			return std::hypot(p[0] - a[0] - dx * t, p[1] - a[1] - dy * t);
		};
		const auto within = [&](const std::unordered_map<Key, std::vector<Segment>>& grid, const P2& p, float extra) {
			const auto it = grid.find(keyOf(p[0], p[1]));
			if (it != grid.end())
				for (const Segment& s : it->second)
					if (distance(p, s.a, s.b) < s.reach + extra)
						return true;
			return false;
		};
		const auto inside = [](const world::Area& a, const P2& p) {
			bool in = false;
			for (const auto& r : a.outer)
				in = in || polygon::Inside(r, p);
			for (const auto& r : a.inner)
				in = in && !polygon::Inside(r, p);
			return in;
		};
		for (const world::Area& a : areas)
		{
			if (a.kind != "cemetery" || a.outer.empty())
				continue;
			float bx0 = 1e30f, by0 = 1e30f, bx1 = -1e30f, by1 = -1e30f;
			for (const auto& r : a.outer)
				for (const P2& p : r)
					bx0 = std::min(bx0, p[0]), by0 = std::min(by0, p[1]), bx1 = std::max(bx1, p[0]), by1 = std::max(by1, p[1]);
			if (bx1 < box[0] || by1 < box[1] || bx0 >= box[2] || by0 >= box[3])
				continue;
			if (MemorialPark(a.name))
				continue; // (flat markers: the lawn)
			++stats.cemeteries;
			// (the rows' way: the outline's edges' directions folded into a right angle, weighed by their length, in 2 degree
			// steps - the heaviest)
			float weights[45] = {};
			for (const auto& r : a.outer)
				for (size_t k = 0; k < r.size(); ++k)
				{
					const P2 &p = r[k], &q = r[(k + 1) % r.size()];
					const float l = std::hypot(q[0] - p[0], q[1] - p[1]);
					float t = std::atan2(q[1] - p[1], q[0] - p[0]);
					t = std::fmod(t + 4 * kPi, kPi / 2);
					weights[std::min(44, static_cast<int>(t / (kPi / 2) * 45))] += l;
				}
			const float turn = (std::max_element(std::begin(weights), std::end(weights)) - std::begin(weights) + 0.5f) * (kPi / 2) / 45;
			// (the face: of the four ways along and across, the most easterly; the rows across it)
			P2 f{std::cos(turn), std::sin(turn)};
			for (int k = 0; k < 4; ++k)
			{
				const P2 g{std::cos(turn + k * kPi / 2), std::sin(turn + k * kPi / 2)};
				if (g[0] > f[0])
					f = g;
			}
			const P2 r{-f[1], f[0]}; // (along the row)
			// the outline's segments, the paths' near it, the props there
			std::unordered_map<Key, std::vector<Segment>> edges, paths;
			for (const auto* rings : {&a.outer, &a.inner})
				for (const auto& ring : *rings)
					for (size_t k = 0; k < ring.size(); ++k)
						addSegment(edges, {ring[k], ring[(k + 1) % ring.size()], 3.0f}); // (a strip of lawn inside the fence)
			for (const world::Walk& wk : walks)
			{
				if (wk.points.size() < 2 || wk.bridge || wk.tunnel)
					continue;
				const float reach = (wk.width > 0 ? wk.width / 2 : 0.9f) + 0.5f;
				for (size_t k = 0; k + 1 < wk.points.size(); ++k)
				{
					const P2 &p = wk.points[k], &q = wk.points[k + 1];
					if (std::max(p[0], q[0]) < bx0 - 5 || std::min(p[0], q[0]) > bx1 + 5 || std::max(p[1], q[1]) < by0 - 5 || std::min(p[1], q[1]) > by1 + 5)
						continue;
					addSegment(paths, {p, q, reach});
				}
			}
			std::unordered_map<Key, std::vector<P2>> taken;
			for (const Prop& p : out)
				if (p.position[0] >= bx0 - 4 && p.position[0] <= bx1 + 4 && p.position[1] >= by0 - 4 && p.position[1] <= by1 + 4)
					taken[keyOf(p.position[0], p.position[1])].push_back({p.position[0], p.position[1]});
			const auto clearOf = [&](const P2& c, float reach) {
				for (int dj = -1; dj <= 1; ++dj)
					for (int di = -1; di <= 1; ++di)
						if (const auto it = taken.find(keyOf(c[0] + di * 8.0f, c[1] + dj * 8.0f)); it != taken.end())
							for (const P2& q : it->second)
								if (std::hypot(q[0] - c[0], q[1] - c[1]) < reach)
									return false;
				return true;
			};
			// the plots: (i along the row, j across) from the box's corners in those axes, whole multiples of the plot and
			// the row - the same in every tile
			float s0 = 1e30f, s1 = -1e30f, t0 = 1e30f, t1 = -1e30f;
			for (const P2& p : {P2{bx0, by0}, P2{bx1, by0}, P2{bx0, by1}, P2{bx1, by1}})
			{
				const float s = p[0] * r[0] + p[1] * r[1], t = p[0] * f[0] + p[1] * f[1];
				s0 = std::min(s0, s), s1 = std::max(s1, s), t0 = std::min(t0, t), t1 = std::max(t1, t);
			}
			const int i0 = static_cast<int>(std::floor(s0 / kPlot)), i1 = static_cast<int>(std::ceil(s1 / kPlot));
			const int j0 = static_cast<int>(std::floor(t0 / kRow)), j1 = static_cast<int>(std::ceil(t1 / kRow));
			std::unordered_map<Key, bool> used;
			const auto plotKey = [](int i, int j) { return static_cast<Key>(i) << 32 | static_cast<uint32_t>(j); };
			const auto at = [&](float s, float t) { return P2{r[0] * s + f[0] * t, r[1] * s + f[1] * t}; };
			for (int j = j0; j <= j1; ++j)
				for (int i = i0; i <= i1; ++i)
				{
					if (used.contains(plotKey(i, j)))
						continue;
					const P2 c0 = at((i + 0.5f) * kPlot, (j + 0.5f) * kRow);
					if (!In(box, c0[0], c0[1]) || !inside(a, c0))
						continue;
					// (taken: in patches - a smooth noise of 35 m)
					const uint32_t h = Mix(static_cast<uint32_t>(i) * 73856093u ^ static_cast<uint32_t>(j) * 19349663u ^ 0x9e3779b9u);
					const float patch = 0.5f + 0.25f * std::sin(c0[0] * 0.18f + std::sin(c0[1] * 0.11f) * 1.9f) + 0.25f * std::sin(c0[1] * 0.16f + std::sin(c0[0] * 0.09f) * 2.3f);
					const float share = 0.7f * (0.45f + 0.55f * patch);
					if (static_cast<float>(h % 1000) >= share * 1000)
						continue;
					// (a kind by its weight; when it does not fit, the single plot's pillar, else none)
					int pick = static_cast<int>((h >> 10) % static_cast<uint32_t>(total));
					size_t kind = 0;
					while (pick >= kKinds[kind].weight)
						pick -= kKinds[kind].weight, ++kind;
					for (const size_t tried : {kind, size_t{6}})
					{
						const Kind& k = kKinds[tried];
						bool free = true;
						for (int dj = 0; dj < k.rows && free; ++dj)
							for (int di = 0; di < k.plots && free; ++di)
								free = !used.contains(plotKey(i + di, j + dj));
						if (!free)
							continue;
						// (its middle: along the row in the middle of its plots; across, a headstone at the row's west end - the
						// grave east of it -, a tomb's or a monument's middle as deep as it is)
						const float s = (i + k.plots * 0.5f) * kPlot;
						const float t = j * kRow + (k.depth < 1.2f ? 0.5f : 0.3f + k.depth / 2);
						const P2 c = at(s, t);
						bool fits = true;
						for (const float u : {-0.5f, 0.0f, 0.5f})
							for (const float v : {-0.5f, 0.0f, 0.5f})
							{
								const P2 q = at(s + u * k.width, t + v * k.depth);
								fits = fits && inside(a, q) && open(q[0], q[1]) && !within(edges, q, 0) && !within(paths, q, 0);
							}
						if (!fits || !clearOf(c, std::max(1.3f, 0.5f * std::max(k.width, k.depth) + 0.6f)))
							continue;
						float z = ground(c[0], c[1]);
						for (const float u : {-0.5f, 0.5f})
							for (const float v : {-0.5f, 0.5f})
							{
								const P2 q = at(s + u * k.width, t + v * k.depth);
								z = std::min(z, ground(q[0], q[1]));
							}
						// (the model's x along r, its y along -f - its face, -y, the east-most way -; its origin from its box's
						// middle)
						const P2 mx{r[0], r[1]}, my{-f[0], -f[1]};
						const P2 o{c[0] - mx[0] * k.centre[0] - my[0] * k.centre[1], c[1] - mx[1] * k.centre[0] - my[1] * k.centre[1]};
						out.push_back({loader::convert::Joaat(k.name), {o[0], o[1], z - 0.03f}, std::atan2(mx[1], mx[0]), 150});
						for (int dj = 0; dj < k.rows; ++dj)
							for (int di = 0; di < k.plots; ++di)
								used[plotKey(i + di, j + dj)] = true;
						++stats.graves;
						break;
					}
				}
		}
	}
}
