#include "props.hpp"

#include <algorithm>
#include <cmath>

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

		float Length(const roads::Edge& e)
		{
			float l = 0;
			for (size_t i = 0; i + 1 < e.points.size(); ++i)
				l += std::hypot(e.points[i + 1][0] - e.points[i][0], e.points[i + 1][1] - e.points[i][1]);
			return l;
		}
	}

	void Place(const world::World& w, const roads::Network& n, const std::function<bool(float, float)>& open,
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
			const float total = Length(e);
			const float side = e.HalfWidth() + 0.6f; // (on the sidewalk, near the kerb)
			// street lights every 40 m on the right of the way (both sides on wide roads), their arm (the model's -y)
			// over the road
			for (float s = 12; s < total - 12; s += 40)
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
				for (float s = 20; s < total - 20; s += 30)
				{
					P2 p, d;
					float z;
					if (!At(e, s, p, d, z))
						break;
					const float sign = static_cast<int>(s / 30) % 2 ? 1.0f : -1.0f;
					const P2 right{d[1], -d[0]};
					const float o = e.HalfWidth() + roads::kSidewalk * 0.65f;
					const P2 q{p[0] + right[0] * o * sign, p[1] + right[1] * o * sign};
					if (In(box, q[0], q[1]))
						addPalm(q[0], q[1], z + roads::kKerb + 0.2f);
				}
		}
		// traffic signal masts: at a signalled junction, at the right of each road coming in, the arm (the model's -x)
		// across its lanes
		for (const auto& v : n.vertices)
		{
			if (!v.signals || !In(box, v.p[0], v.p[1]))
				continue;
			for (const int ei : v.edges)
			{
				const roads::Edge& e = n.edges[ei];
				const bool atB = &n.vertices[e.b] == &v;
				const int lanesIn = atB ? e.forward : e.backward;
				if (!lanesIn || e.points.size() < 2)
					continue;
				// the incoming direction near the junction, and the stop line's distance back from it
				const P2& a = atB ? e.points[e.points.size() - 2] : e.points[1];
				const P2& b = atB ? e.points.back() : e.points.front();
				const float l = std::hypot(b[0] - a[0], b[1] - a[1]);
				if (l < 1)
					continue;
				const P2 d{(b[0] - a[0]) / l, (b[1] - a[1]) / l};
				float reach = 0;
				for (const int ej : v.edges)
					reach = std::max(reach, n.edges[ej].HalfWidth() + (n.edges[ej].sidewalks ? roads::kSidewalk : 0));
				const float back = std::min(reach * 1.15f + 0.5f, l * 0.9f);
				const P2 right{d[1], -d[0]};
				const float o = e.HalfWidth() + 1.0f;
				const P2 q{b[0] - d[0] * back + right[0] * o, b[1] - d[1] * back + right[1] * o};
				out.push_back({signal, {q[0], q[1], v.z + roads::kKerb}, std::atan2(-d[0], d[1]), 300});
			}
		}
	}
}
