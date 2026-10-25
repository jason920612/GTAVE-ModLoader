#include "roads.hpp"

#include <algorithm>
#include <cmath>
#include <map>
#include <unordered_map>

#include "convert/resource.hpp"

namespace worldgen::roads
{
	namespace
	{
		constexpr float kPi = 3.14159265f;
		constexpr float kTexture = 6.0f; // metres a road / sidewalk texture repeats (the game's own tiles: about 6 m)
		// materials.dat rows
		constexpr uint8_t kTarmac = 4, kConcrete = 1, kPavement = 174;
		// the marking atlas (IM_RoadMarkings02): u ranges of its columns
		constexpr float kWhiteU0 = 0.02f, kWhiteU1 = 0.10f;   // a solid white line
		constexpr float kStripeU0 = 0.135f, kStripeU1 = 0.27f; // a wide white stripe
		constexpr float kDashU0 = 0.28f, kDashU1 = 0.34f;      // a white dash (v 0.15 .. 0.85)
		constexpr float kYellowU0 = 0.41f, kYellowU1 = 0.465f; // a solid yellow line

		uint64_t KeyOf(const P2& p)
		{
			const int32_t x = static_cast<int32_t>(std::llround(p[0] * 100)), y = static_cast<int32_t>(std::llround(p[1] * 100));
			return static_cast<uint64_t>(static_cast<uint32_t>(x)) << 32 | static_cast<uint32_t>(y);
		}
		float Dist(const P2& a, const P2& b) { return std::hypot(b[0] - a[0], b[1] - a[1]); }
		P2 Dir(const P2& a, const P2& b)
		{
			const float l = Dist(a, b);
			return l > 0 ? P2{(b[0] - a[0]) / l, (b[1] - a[1]) / l} : P2{1, 0};
		}
		P2 Left(const P2& d) { return {-d[1], d[0]}; }

		bool Alike(const Edge& x, const Edge& y)
		{
			return x.forward == y.forward && x.backward == y.backward && x.laneWidth == y.laneWidth && x.sidewalks == y.sidewalks && x.bridge == y.bridge &&
			       x.kind == y.kind;
		}

		void Reverse(Edge& e)
		{
			std::reverse(e.points.begin(), e.points.end());
			std::reverse(e.z.begin(), e.z.end());
			std::swap(e.a, e.b);
			std::swap(e.forward, e.backward);
		}

		float Length(const std::vector<P2>& p)
		{
			float l = 0;
			for (size_t i = 0; i + 1 < p.size(); ++i)
				l += Dist(p[i], p[i + 1]);
			return l;
		}

		// A polyline (with heights) between distances s0 and s1 from its start.
		void Cut(const std::vector<P2>& p, const std::vector<float>& z, float s0, float s1, std::vector<P2>& op, std::vector<float>& oz, std::vector<float>& os)
		{
			op.clear(), oz.clear(), os.clear();
			float s = 0;
			for (size_t i = 0; i + 1 < p.size(); ++i)
			{
				const float l = Dist(p[i], p[i + 1]);
				const auto at = [&](float t) {
					return std::make_pair(P2{p[i][0] + (p[i + 1][0] - p[i][0]) * t, p[i][1] + (p[i + 1][1] - p[i][1]) * t}, z[i] + (z[i + 1] - z[i]) * t);
				};
				if (op.empty() && s + l >= s0)
				{
					const auto [q, qz] = at(l > 0 ? (s0 - s) / l : 0);
					op.push_back(q), oz.push_back(qz), os.push_back(s0);
				}
				if (!op.empty() && s + l < s1 && i + 1 < p.size() - 1 && s + l > s0)
					op.push_back(p[i + 1]), oz.push_back(z[i + 1]), os.push_back(s + l);
				if (s + l >= s1)
				{
					const auto [q, qz] = at(l > 0 ? (s1 - s) / l : 0);
					op.push_back(q), oz.push_back(qz), os.push_back(s1);
					return;
				}
				s += l;
			}
		}

		// The left normals along a polyline (mitred at its bends, the mitre at most twice as long).
		std::vector<P2> Normals(const std::vector<P2>& p, std::vector<float>& scale)
		{
			std::vector<P2> n(p.size());
			scale.assign(p.size(), 1.0f);
			for (size_t i = 0; i < p.size(); ++i)
			{
				const P2 d0 = i > 0 ? Dir(p[i - 1], p[i]) : Dir(p[i], p[i + 1]);
				const P2 d1 = i + 1 < p.size() ? Dir(p[i], p[i + 1]) : d0;
				P2 m{d0[0] + d1[0], d0[1] + d1[1]};
				const float l = std::hypot(m[0], m[1]);
				m = l > 1e-4f ? P2{m[0] / l, m[1] / l} : d1;
				n[i] = Left(m);
				const P2 ln = Left(d1);
				scale[i] = 1.0f / std::max(0.5f, n[i][0] * ln[0] + n[i][1] * ln[1]);
			}
			return n;
		}

		struct Builder
		{
			Surfaces& surfaces;

			uint32_t Add(Mesh& m, const P3& p, const P3& normal, const std::array<float, 2>& uv, const std::array<uint8_t, 4>& colour = {255, 12, 255, 255})
			{
				ydr::Vertex v;
				v.position = p;
				v.normal = normal;
				// (the tangent along u: roughly east for planar maps, which is all the shaders need here)
				v.tangent = {1, 0, 0, 1};
				v.colour0 = colour;
				v.uv[0] = uv;
				m.vertices.push_back(v);
				return static_cast<uint32_t>(m.vertices.size() - 1);
			}
			void Tri(Mesh& m, uint32_t a, uint32_t b, uint32_t c) { m.indices.insert(m.indices.end(), {a, b, c}); }
			void Collide(const P3& a, const P3& b, const P3& c, uint8_t material, bool ground)
			{
				const uint32_t base = static_cast<uint32_t>(surfaces.collision.vertices.size());
				surfaces.collision.vertices.insert(surfaces.collision.vertices.end(), {a, b, c});
				surfaces.collision.triangles.push_back({base, base + 1, base + 2});
				surfaces.collision.materials.push_back(material);
				if (ground)
					surfaces.footprint.push_back({a, b, c});
			}
			// A strip between two lines of points (right and left, the same count), anticlockwise from above.
			void Strip(Mesh& m, const std::vector<P3>& right, const std::vector<P3>& left, const std::vector<std::array<float, 2>>& uvRight,
			    const std::vector<std::array<float, 2>>& uvLeft, uint8_t material, bool ground)
			{
				for (size_t i = 0; i + 1 < right.size(); ++i)
				{
					const P3 up{0, 0, 1};
					const uint32_t r0 = Add(m, right[i], up, uvRight[i]), r1 = Add(m, right[i + 1], up, uvRight[i + 1]), l1 = Add(m, left[i + 1], up, uvLeft[i + 1]),
					               l0 = Add(m, left[i], up, uvLeft[i]);
					Tri(m, r0, r1, l1);
					Tri(m, r0, l1, l0);
					if (material)
					{
						Collide(right[i], right[i + 1], left[i + 1], material, ground);
						Collide(right[i], left[i + 1], left[i], material, ground);
					}
				}
			}
			// A vertical face from `bottom` up `height`, facing to the left of the line's direction when `faceLeft`.
			void Wall(Mesh& m, const std::vector<P3>& bottom, float height, bool faceLeft, uint8_t material)
			{
				float s = 0;
				for (size_t i = 0; i + 1 < bottom.size(); ++i)
				{
					const P2 d = Dir({bottom[i][0], bottom[i][1]}, {bottom[i + 1][0], bottom[i + 1][1]});
					const P2 n = faceLeft ? Left(d) : P2{-Left(d)[0], -Left(d)[1]};
					const float l = Dist({bottom[i][0], bottom[i][1]}, {bottom[i + 1][0], bottom[i + 1][1]});
					const P3 b0 = bottom[i], b1 = bottom[i + 1], t0{b0[0], b0[1], b0[2] + height}, t1{b1[0], b1[1], b1[2] + height};
					const P3 normal{n[0], n[1], 0};
					const uint32_t i0 = Add(m, b0, normal, {s / kTexture, 0}), i1 = Add(m, b1, normal, {(s + l) / kTexture, 0}),
					               i2 = Add(m, t1, normal, {(s + l) / kTexture, height / 0.6f}), i3 = Add(m, t0, normal, {s / kTexture, height / 0.6f});
					if (faceLeft)
						Tri(m, i0, i2, i1), Tri(m, i0, i3, i2);
					else
						Tri(m, i0, i1, i2), Tri(m, i0, i2, i3);
					if (material)
					{
						if (faceLeft)
							Collide(b0, t1, b1, material, false), Collide(b0, t0, t1, material, false);
						else
							Collide(b0, b1, t1, material, false), Collide(b0, t1, t0, material, false);
					}
					s += l;
				}
			}
			// A flat polygon (a fan from `centre`, the points anticlockwise round it), textured in world space.
			void Fan(Mesh& m, const P3& centre, const std::vector<P3>& ring, uint8_t material, bool ground)
			{
				const P3 up{0, 0, 1};
				const uint32_t c = Add(m, centre, up, {centre[0] / kTexture, -centre[1] / kTexture});
				std::vector<uint32_t> r;
				for (const P3& p : ring)
					r.push_back(Add(m, p, up, {p[0] / kTexture, -p[1] / kTexture}));
				for (size_t i = 0; i < ring.size(); ++i)
				{
					const size_t j = (i + 1) % ring.size();
					Tri(m, c, r[i], r[j]);
					if (material)
						Collide(centre, ring[i], ring[j], material, ground);
				}
			}
			// A marking ribbon along a polyline at offset `o` (left positive), `width` wide, from the atlas column u0..u1
			// repeating every `repeat` metres along v; dashed: `dash` metres every `period`.
			void Marking(const std::vector<P2>& p, const std::vector<float>& z, const std::vector<float>& sAlong, float o, float width, float u0, float u1,
			    float repeat, float dash = 0, float period = 0, float v0 = 0, float v1 = 1)
			{
				std::vector<float> scale;
				const auto n = Normals(p, scale);
				const auto at = [&](float s, P3& l, P3& r) {
					// the point at distance s (interpolated), its ribbon corners
					size_t i = 0;
					while (i + 2 < sAlong.size() && sAlong[i + 1] < s)
						++i;
					const float span = sAlong[i + 1] - sAlong[i], t = span > 0 ? std::clamp((s - sAlong[i]) / span, 0.0f, 1.0f) : 0;
					const P2 q{p[i][0] + (p[i + 1][0] - p[i][0]) * t, p[i][1] + (p[i + 1][1] - p[i][1]) * t};
					const P2 nn = Dir({0, 0}, {n[i][0] + (n[i + 1][0] - n[i][0]) * t, n[i][1] + (n[i + 1][1] - n[i][1]) * t});
					const float qz = z[i] + (z[i + 1] - z[i]) * t + 0.03f;
					l = {q[0] + nn[0] * (o + width / 2), q[1] + nn[1] * (o + width / 2), qz};
					r = {q[0] + nn[0] * (o - width / 2), q[1] + nn[1] * (o - width / 2), qz};
				};
				const float s0 = sAlong.front(), s1 = sAlong.back();
				const std::array<uint8_t, 4> colour{255, 253, 0, 255};
				const P3 up{0, 0, 1};
				Mesh& m = surfaces.markings;
				if (dash > 0)
				{
					for (float s = s0 + (period - dash) / 2; s + dash <= s1; s += period)
					{
						P3 l0, r0, l1, r1;
						at(s, l0, r0);
						at(s + dash, l1, r1);
						const uint32_t a = Add(m, r0, up, {u0, v0}, colour), b = Add(m, r1, up, {u0, v1}, colour), c = Add(m, l1, up, {u1, v1}, colour),
						               d = Add(m, l0, up, {u1, v0}, colour);
						Tri(m, a, b, c), Tri(m, a, c, d);
					}
					return;
				}
				P3 lp, rp;
				at(s0, lp, rp);
				for (float s = s0; s < s1 - 1e-3f;)
				{
					const float next = std::min(s1, s + 6.0f);
					P3 l, r;
					at(next, l, r);
					const uint32_t a = Add(m, rp, up, {u0, s / repeat}, colour), b = Add(m, r, up, {u0, next / repeat}, colour),
					               c = Add(m, l, up, {u1, next / repeat}, colour), d = Add(m, lp, up, {u1, s / repeat}, colour);
					Tri(m, a, b, c), Tri(m, a, c, d);
					lp = l, rp = r, s = next;
				}
			}
		};
	}

	Network Build(const world::World& w)
	{
		Network n;
		// vertices: road ends and points shared by roads (or met twice by one)
		std::unordered_map<uint64_t, int> seen;
		for (const auto& r : w.roads)
		{
			for (const P2& p : r.points)
				++seen[KeyOf(p)];
			seen[KeyOf(r.points.front())] += 2, seen[KeyOf(r.points.back())] += 2;
		}
		std::unordered_map<uint64_t, int> vertexOf;
		const auto vertex = [&](const P2& p) {
			const uint64_t k = KeyOf(p);
			if (const auto it = vertexOf.find(k); it != vertexOf.end())
				return it->second;
			n.vertices.push_back({p});
			return vertexOf[k] = static_cast<int>(n.vertices.size() - 1);
		};
		for (const auto& r : w.roads)
		{
			Edge e;
			e.forward = r.forward, e.backward = r.backward, e.laneWidth = r.laneWidth, e.sidewalks = r.sidewalks, e.bridge = r.bridge, e.kind = r.kind,
			e.name = r.name, e.speed = r.speed;
			e.a = vertex(r.points.front());
			e.points.push_back(r.points.front());
			for (size_t i = 1; i < r.points.size(); ++i)
			{
				if (r.points[i] == e.points.back())
					continue;
				e.points.push_back(r.points[i]);
				if (seen[KeyOf(r.points[i])] >= 2)
				{
					e.b = vertex(r.points[i]);
					if (e.a != e.b || e.points.size() > 2)
						n.edges.push_back(e);
					e.points = {r.points[i]};
					e.a = e.b;
				}
			}
		}
		for (size_t i = 0; i < n.edges.size(); ++i)
		{
			n.vertices[n.edges[i].a].edges.push_back(static_cast<int>(i));
			n.vertices[n.edges[i].b].edges.push_back(static_cast<int>(i));
		}
		// chains of alike pieces joined (a vertex between exactly two of them goes)
		std::vector<bool> deadEdge(n.edges.size()), deadVertex(n.vertices.size());
		for (size_t v = 0; v < n.vertices.size(); ++v)
		{
			auto& list = n.vertices[v].edges;
			if (list.size() != 2 || list[0] == list[1])
				continue;
			Edge& e1 = n.edges[list[0]];
			Edge e2 = n.edges[list[1]];
			if (e1.b != static_cast<int>(v))
				Reverse(e1);
			if (e2.a != static_cast<int>(v))
				Reverse(e2);
			if (e1.a == e1.b || e2.a == e2.b || !Alike(e1, e2) || e1.a == e2.b)
				continue;
			e1.points.insert(e1.points.end(), e2.points.begin() + 1, e2.points.end());
			e1.b = e2.b;
			auto& far = n.vertices[e2.b].edges;
			std::replace(far.begin(), far.end(), list[1], list[0]);
			deadEdge[list[1]] = true;
			deadVertex[v] = true;
			list.clear();
		}
		// compact
		std::vector<int> vMap(n.vertices.size(), -1), eMap(n.edges.size(), -1);
		Network out;
		for (size_t v = 0; v < n.vertices.size(); ++v)
			if (!deadVertex[v])
				vMap[v] = static_cast<int>(out.vertices.size()), out.vertices.push_back({n.vertices[v].p});
		for (size_t e = 0; e < n.edges.size(); ++e)
			if (!deadEdge[e])
			{
				Edge ed = n.edges[e];
				ed.a = vMap[ed.a], ed.b = vMap[ed.b];
				if (ed.a < 0 || ed.b < 0 || Length(ed.points) < 0.5f)
					continue;
				eMap[e] = static_cast<int>(out.edges.size());
				out.vertices[ed.a].edges.push_back(eMap[e]);
				out.vertices[ed.b].edges.push_back(eMap[e]);
				out.edges.push_back(std::move(ed));
			}
		// traffic signals: at the junction nearest each signal node (within 25 m)
		for (const auto& p : w.points)
		{
			if (p.kind != "traffic_signals")
				continue;
			int best = -1;
			float bestD = 25;
			for (size_t v = 0; v < out.vertices.size(); ++v)
				if (out.vertices[v].edges.size() >= 3)
					if (const float d = Dist(out.vertices[v].p, p.position); d < bestD)
						bestD = d, best = static_cast<int>(v);
			if (best >= 0)
				out.vertices[best].signals = true;
		}
		return out;
	}

	void Heights(Network& n, const std::function<float(float, float)>& ground, float clearance)
	{
		for (Edge& e : n.edges)
		{
			e.z.resize(e.points.size());
			const float total = Length(e.points);
			const float za = std::max(ground(e.points.front()[0], e.points.front()[1]) + 0.05f, 0.6f);
			const float zb = std::max(ground(e.points.back()[0], e.points.back()[1]) + 0.05f, 0.6f);
			float s = 0;
			for (size_t i = 0; i < e.points.size(); ++i)
			{
				if (i)
					s += Dist(e.points[i - 1], e.points[i]);
				const float g = ground(e.points[i][0], e.points[i][1]);
				if (!e.bridge)
				{
					e.z[i] = std::max(g + 0.05f, 0.6f);
					continue;
				}
				// a bridge: from its ends' heights, arched up to the clearance over its middle (less over a short one)
				const float t = total > 0 ? s / total : 0;
				const float rise = std::min(clearance, total * 0.08f) * std::sin(kPi * t);
				e.z[i] = std::max(za + (zb - za) * t + rise, g + 0.05f);
			}
		}
		// a vertex at its edges' mean end height; the ends moved there
		for (Vertex& v : n.vertices)
		{
			float sum = 0;
			int count = 0;
			for (const int ei : v.edges)
			{
				const Edge& e = n.edges[ei];
				if (&n.vertices[e.a] == &v)
					sum += e.z.front(), ++count;
				if (&n.vertices[e.b] == &v)
					sum += e.z.back(), ++count;
			}
			v.z = count ? sum / count : 0;
		}
		for (Edge& e : n.edges)
		{
			e.z.front() = n.vertices[e.a].z;
			e.z.back() = n.vertices[e.b].z;
		}
	}

	void Geometry(const Network& n, Surfaces& out)
	{
		Builder b{out};
		// how far each edge stops short of its junctions
		std::vector<float> trimA(n.edges.size(), 0), trimB(n.edges.size(), 0);
		std::vector<bool> junction(n.vertices.size());
		for (size_t v = 0; v < n.vertices.size(); ++v)
		{
			const auto& list = n.vertices[v].edges;
			junction[v] = list.size() >= 3 || (list.size() == 2 && (list[0] == list[1] || !Alike(n.edges[list[0]], n.edges[list[1]]) ||
			                                                          n.edges[list[0]].HalfWidth() != n.edges[list[1]].HalfWidth()));
			if (!junction[v])
				continue;
			float reach = 0;
			for (const int ei : list)
				reach = std::max(reach, n.edges[ei].HalfWidth() + (n.edges[ei].sidewalks ? kSidewalk : 0));
			for (const int ei : list)
			{
				const Edge& e = n.edges[ei];
				const float d = std::min(reach * 1.15f + 0.5f, 0.45f * Length(e.points));
				if (e.a == static_cast<int>(v))
					trimA[ei] = d;
				if (e.b == static_cast<int>(v))
					trimB[ei] = d;
			}
		}
		// edge ends at junctions: (vertex, outward direction angle, road corners right / left, sidewalk corners, z, sidewalks)
		struct End
		{
			int edge;
			float angle;
			P3 roadR, roadL, walkR, walkL;
			bool sidewalks;
		};
		std::vector<std::vector<End>> ends(n.vertices.size());
		for (size_t ei = 0; ei < n.edges.size(); ++ei)
		{
			const Edge& e = n.edges[ei];
			const float total = Length(e.points);
			std::vector<P2> p;
			std::vector<float> z, s;
			Cut(e.points, e.z, trimA[ei], total - trimB[ei], p, z, s);
			if (p.size() < 2)
				continue;
			std::vector<float> scale;
			const auto nrm = Normals(p, scale);
			const float half = e.HalfWidth();
			std::vector<P3> right, left, walkRi, walkRo, walkLi, walkLo;
			std::vector<std::array<float, 2>> uvR, uvL, uvWi, uvWo;
			for (size_t i = 0; i < p.size(); ++i)
			{
				const float k = scale[i];
				const auto off = [&](float o, float dz) { return P3{p[i][0] + nrm[i][0] * o * k, p[i][1] + nrm[i][1] * o * k, z[i] + dz}; };
				right.push_back(off(-half, 0));
				left.push_back(off(half, 0));
				uvR.push_back({-half / kTexture, s[i] / kTexture});
				uvL.push_back({half / kTexture, s[i] / kTexture});
				walkRi.push_back(off(-half, kKerb));
				walkRo.push_back(off(-half - kSidewalk, kKerb));
				walkLi.push_back(off(half, kKerb));
				walkLo.push_back(off(half + kSidewalk, kKerb));
				uvWi.push_back({0, s[i] / kTexture});
				uvWo.push_back({kSidewalk / kTexture, s[i] / kTexture});
			}
			b.Strip(out.asphalt, right, left, uvR, uvL, kTarmac, !e.bridge);
			if (e.sidewalks)
			{
				b.Strip(out.sidewalk, walkLi, walkLo, uvWi, uvWo, kPavement, !e.bridge); // (left: inner on the right of its direction)
				b.Strip(out.sidewalk, walkRo, walkRi, uvWo, uvWi, kPavement, !e.bridge);
				b.Wall(out.kerb, left, kKerb, false, kConcrete);  // facing the road (to the right of the left kerb's direction)
				b.Wall(out.kerb, right, kKerb, true, kConcrete);
				// the sidewalks' outer edges down to the ground below them
				std::vector<P3> lo = walkLo, ro = walkRo;
				for (P3& q : lo)
					q[2] -= kKerb + 0.25f;
				for (P3& q : ro)
					q[2] -= kKerb + 0.25f;
				b.Wall(out.kerb, lo, kKerb + 0.25f, true, 0);
				b.Wall(out.kerb, ro, kKerb + 0.25f, false, 0);
			}
			// markings: between the directions (double yellow), between lanes (dashed white), edge lines on the big roads
			const float w = e.laneWidth, base = -half + 0.3f;
			if (e.forward && e.backward)
			{
				const float o = base + e.forward * w;
				b.Marking(p, z, s, o + 0.12f, 0.16f, kYellowU0, kYellowU1, 4.0f);
				b.Marking(p, z, s, o - 0.12f, 0.16f, kYellowU0, kYellowU1, 4.0f);
			}
			for (int k = 1; k < e.forward; ++k)
				b.Marking(p, z, s, base + k * w, 0.2f, kDashU0, kDashU1, 4.0f, 3.0f, 12.0f, 0.15f, 0.85f);
			for (int k = 1; k < e.backward; ++k)
				b.Marking(p, z, s, half - 0.3f - k * w, 0.2f, kDashU0, kDashU1, 4.0f, 3.0f, 12.0f, 0.15f, 0.85f);
			if (e.kind.starts_with("motorway") || e.kind.starts_with("trunk") || e.kind.starts_with("primary") || e.kind.starts_with("secondary"))
			{
				b.Marking(p, z, s, -half + 0.45f, 0.3f, kWhiteU0, kWhiteU1, 4.0f);
				b.Marking(p, z, s, half - 0.45f, 0.3f, kWhiteU0, kWhiteU1, 4.0f);
			}
			// stop lines where the lanes come into a junction (the forward lanes at b, the backward ones at a)
			const auto stop = [&](bool atB, int lanes) {
				if (!lanes || p.size() < 2)
					return;
				const size_t i = atB ? p.size() - 1 : 0, j = atB ? p.size() - 2 : 1;
				const P2 d = Dir(p[j], p[i]); // towards the junction
				const P2 l = Left(atB ? d : P2{-d[0], -d[1]});
				const float back = 1.2f;
				const P2 c{p[i][0] - d[0] * back, p[i][1] - d[1] * back};
				const float o0 = atB ? base : half - 0.3f - lanes * w, o1 = atB ? base + lanes * w : half - 0.3f;
				std::vector<P2> line{{c[0] + l[0] * o0, c[1] + l[1] * o0}, {c[0] + l[0] * o1, c[1] + l[1] * o1}};
				std::vector<float> lz{z[i], z[i]}, ls{0, o1 - o0};
				b.Marking(line, lz, ls, 0, 0.5f, kStripeU0, kStripeU1, 2.0f);
			};
			if (junction[e.b] && n.vertices[e.b].edges.size() >= 3)
				stop(true, e.forward);
			if (junction[e.a] && n.vertices[e.a].edges.size() >= 3)
				stop(false, e.backward);
			// this edge's ends at junctions
			for (const bool atA : {true, false})
			{
				const int v = atA ? e.a : e.b;
				if (!junction[v])
					continue;
				const size_t i = atA ? 0 : p.size() - 1, j = atA ? 1 : p.size() - 2;
				const P2 outward = Dir(p[i], p[j]);
				const P2 l = Left(outward);
				const auto at = [&](float o, float dz) { return P3{p[i][0] + l[0] * o, p[i][1] + l[1] * o, z[i] + dz}; };
				ends[v].push_back({static_cast<int>(ei), std::atan2(outward[1], outward[0]), at(-half, 0), at(half, 0), at(-half - kSidewalk, kKerb),
				    at(half + kSidewalk, kKerb), e.sidewalks});
			}
		}
		// junctions: the asphalt between the ends, the sidewalk corners between neighbouring ends
		for (size_t v = 0; v < n.vertices.size(); ++v)
		{
			auto& list = ends[v];
			if (list.size() < 2)
				continue;
			std::sort(list.begin(), list.end(), [](const End& x, const End& y) { return x.angle < y.angle; });
			std::vector<P3> ring;
			for (const End& e : list)
				ring.push_back(e.roadR), ring.push_back(e.roadL);
			const P3 centre{n.vertices[v].p[0], n.vertices[v].p[1], n.vertices[v].z};
			b.Fan(out.asphalt, centre, ring, kTarmac, true);
			for (size_t k = 0; k < list.size(); ++k)
			{
				const End& x = list[k];
				const End& y = list[(k + 1) % list.size()];
				if (!x.sidewalks && !y.sidewalks)
					continue;
				// the corner from x's left side round to y's right side, if they are less than half a turn apart
				float gap = y.angle - x.angle;
				if (gap <= 0)
					gap += 2 * kPi;
				if (gap > kPi * 0.95f)
					continue;
				const P3 xi{x.roadL[0], x.roadL[1], x.roadL[2] + kKerb}, yi{y.roadR[0], y.roadR[1], y.roadR[2] + kKerb};
				const P3 mid{(x.walkL[0] + y.walkR[0]) / 2, (x.walkL[1] + y.walkR[1]) / 2, (x.walkL[2] + y.walkR[2]) / 2};
				// (the outer corner pushed out to where the two sidewalks' outer edges would meet)
				const P3 corner{mid[0] + (mid[0] - (xi[0] + yi[0]) / 2) * 0.4f, mid[1] + (mid[1] - (xi[1] + yi[1]) / 2) * 0.4f, mid[2]};
				const P3 c{(xi[0] + yi[0] + x.walkL[0] + y.walkR[0]) / 4, (xi[1] + yi[1] + x.walkL[1] + y.walkR[1]) / 4, xi[2]};
				b.Fan(out.sidewalk, c, {xi, x.walkL, corner, y.walkR, yi}, kPavement, true);
				b.Wall(out.kerb, {x.roadL, y.roadR}, kKerb, true, kConcrete);
			}
		}
	}

	void Paths(const Network& n, float spacing, std::vector<PathNode>& nodes, std::vector<PathLink>& links)
	{
		const auto speedOf = [](const Edge& e) {
			if (e.speed > 0)
				return e.speed;
			if (e.kind.starts_with("motorway"))
				return 29.0f;
			if (e.kind.starts_with("trunk") || e.kind.starts_with("primary"))
				return 18.0f;
			if (e.kind.starts_with("secondary") || e.kind.starts_with("tertiary"))
				return 15.0f;
			return 11.0f;
		};
		const auto densityOf = [](const Edge& e) {
			return e.kind.starts_with("motorway") || e.kind.starts_with("primary") || e.kind.starts_with("trunk") ? 5 : e.kind == "service" ? 1 : 3;
		};
		std::vector<int> vertexNode(n.vertices.size(), -1);
		for (size_t v = 0; v < n.vertices.size(); ++v)
		{
			if (n.vertices[v].edges.empty())
				continue;
			PathNode node;
			node.p = {n.vertices[v].p[0], n.vertices[v].p[1], n.vertices[v].z};
			node.junction = n.vertices[v].edges.size() >= 3;
			const Edge& first = n.edges[n.vertices[v].edges[0]];
			node.speed = speedOf(first);
			node.density = densityOf(first);
			node.street = loader::convert::Joaat(first.name);
			vertexNode[v] = static_cast<int>(nodes.size());
			nodes.push_back(node);
		}
		for (const Edge& e : n.edges)
		{
			const float total = Length(e.points);
			const int steps = std::max(1, static_cast<int>(std::ceil(total / spacing)));
			int previous = vertexNode[e.a];
			float s = 0;
			size_t i = 0;
			float segStart = 0;
			for (int k = 1; k < steps; ++k)
			{
				const float want = total * k / steps;
				while (i + 1 < e.points.size() && segStart + Dist(e.points[i], e.points[i + 1]) < want)
					segStart += Dist(e.points[i], e.points[i + 1]), ++i;
				const float l = Dist(e.points[i], e.points[i + 1]), t = l > 0 ? (want - segStart) / l : 0;
				PathNode node;
				node.p = {e.points[i][0] + (e.points[i + 1][0] - e.points[i][0]) * t, e.points[i][1] + (e.points[i + 1][1] - e.points[i][1]) * t,
				    e.z[i] + (e.z[i + 1] - e.z[i]) * t};
				node.speed = speedOf(e);
				node.density = densityOf(e);
				node.street = loader::convert::Joaat(e.name);
				nodes.push_back(node);
				const int here = static_cast<int>(nodes.size() - 1);
				links.push_back({previous, here, e.forward, e.backward, e.HalfWidth() * 2});
				previous = here;
				(void)s;
			}
			links.push_back({previous, vertexNode[e.b], e.forward, e.backward, e.HalfWidth() * 2});
		}
	}
}
