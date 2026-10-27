#include "props.hpp"

#include <algorithm>
#include <cmath>
#include <string_view>
#include <vector>

#include "convert/resource.hpp"

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
}
