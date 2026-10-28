#include "roads.hpp"

#include <algorithm>
#include <cmath>
#include <map>
#include <unordered_map>

#include "convert/resource.hpp"
#include "polygon.hpp"

namespace worldgen::roads
{
	bool Minor(const Edge& e)
	{
		return e.kind == "service" || e.kind == "track" || e.kind == "footway" || e.kind == "path" || e.kind == "cycleway" || e.kind == "pedestrian" ||
		       e.kind == "steps";
	}

	namespace
	{
		constexpr float kAsphaltCell = 8;
		int64_t AsphaltKey(float x, float y)
		{
			return static_cast<int64_t>(std::floor(x / kAsphaltCell)) << 32 | static_cast<uint32_t>(static_cast<int>(std::floor(y / kAsphaltCell)));
		}
	}

	Asphalt::Asphalt(const Collision& c, float reach) : col(c)
	{
		// (each triangle in every cell within `reach` of it: a point's own cell then has all it can be near)
		for (uint32_t t = 0; t < col.triangles.size(); ++t)
		{
			if (col.kinds[t] != 1)
				continue;
			float x0 = 1e30f, y0 = 1e30f, x1 = -1e30f, y1 = -1e30f;
			for (const uint32_t k : col.triangles[t])
				x0 = std::min(x0, col.vertices[k][0]), y0 = std::min(y0, col.vertices[k][1]), x1 = std::max(x1, col.vertices[k][0]),
				y1 = std::max(y1, col.vertices[k][1]);
			for (float y = std::floor((y0 - reach) / kAsphaltCell) * kAsphaltCell; y <= y1 + reach; y += kAsphaltCell)
				for (float x = std::floor((x0 - reach) / kAsphaltCell) * kAsphaltCell; x <= x1 + reach; x += kAsphaltCell)
					cells[AsphaltKey(x + kAsphaltCell / 2, y + kAsphaltCell / 2)].push_back(t);
		}
	}

	bool Asphalt::Within(float x, float y, float r) const
	{
		const auto it = cells.find(AsphaltKey(x, y));
		if (it == cells.end())
			return false;
		for (const uint32_t t : it->second)
		{
			const P3* v[3] = {&col.vertices[col.triangles[t][0]], &col.vertices[col.triangles[t][1]], &col.vertices[col.triangles[t][2]]};
			// (inside: on the same side of all three edges; else the nearest edge within r)
			float side[3];
			for (int k = 0; k < 3; ++k)
			{
				const P3 &p = *v[k], &q = *v[(k + 1) % 3];
				side[k] = (q[0] - p[0]) * (y - p[1]) - (q[1] - p[1]) * (x - p[0]);
			}
			if ((side[0] >= 0 && side[1] >= 0 && side[2] >= 0) || (side[0] <= 0 && side[1] <= 0 && side[2] <= 0))
				return true;
			if (r <= 0)
				continue;
			for (int k = 0; k < 3; ++k)
			{
				const P3 &p = *v[k], &q = *v[(k + 1) % 3];
				const float dx = q[0] - p[0], dy = q[1] - p[1], len2 = dx * dx + dy * dy;
				const float s = len2 > 0 ? std::clamp(((x - p[0]) * dx + (y - p[1]) * dy) / len2, 0.0f, 1.0f) : 0.0f;
				if (std::hypot(p[0] + s * dx - x, p[1] + s * dy - y) < r)
					return true;
			}
		}
		return false;
	}

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
			       x.walkWidth == y.walkWidth && x.parking == y.parking && x.bike == y.bike && x.curb == y.curb && x.walkLeft == y.walkLeft &&
			       x.walkRight == y.walkRight &&
			       x.layer == y.layer &&
			       x.kind == y.kind && paving::Of(x.surface) == paving::Of(y.surface);
		}

		void Reverse(Edge& e)
		{
			std::reverse(e.points.begin(), e.points.end());
			std::reverse(e.z.begin(), e.z.end());
			std::swap(e.a, e.b);
			std::swap(e.forward, e.backward);
			std::swap(e.walkLeft, e.walkRight);
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
			void Collide(const P3& a, const P3& b, const P3& c, uint8_t material, bool ground, uint8_t kind = 0)
			{
				const uint32_t base = static_cast<uint32_t>(surfaces.collision.vertices.size());
				surfaces.collision.vertices.insert(surfaces.collision.vertices.end(), {a, b, c});
				surfaces.collision.triangles.push_back({base, base + 1, base + 2});
				surfaces.collision.materials.push_back(material);
				surfaces.collision.kinds.push_back(kind);
				if (ground)
					surfaces.footprint.push_back({a, b, c});
			}
			// A strip between two lines of points (right and left, the same count), anticlockwise from above.
			void Strip(Mesh& m, const std::vector<P3>& right, const std::vector<P3>& left, const std::vector<std::array<float, 2>>& uvRight,
			    const std::vector<std::array<float, 2>>& uvLeft, uint8_t material, bool ground, uint8_t kind = 0)
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
						Collide(right[i], right[i + 1], left[i + 1], material, ground, kind);
						Collide(right[i], left[i + 1], left[i], material, ground, kind);
					}
				}
			}
			// A vertical face from `bottom` up `height`, facing to the left of the line's direction when `faceLeft`.
			// `row`: where in the kerb picture its strip starts (kPlainRow the plain concrete; kYellowRow the yellow paint)
			void Wall(Mesh& m, const std::vector<P3>& bottom, float height, bool faceLeft, uint8_t material, float row = 0.81f)
			{
				float s = 0;
				for (size_t i = 0; i + 1 < bottom.size(); ++i)
				{
					const P2 d = Dir({bottom[i][0], bottom[i][1]}, {bottom[i + 1][0], bottom[i + 1][1]});
					const P2 n = faceLeft ? Left(d) : P2{-Left(d)[0], -Left(d)[1]};
					const float l = Dist({bottom[i][0], bottom[i][1]}, {bottom[i + 1][0], bottom[i + 1][1]});
					const P3 b0 = bottom[i], b1 = bottom[i + 1], t0{b0[0], b0[1], b0[2] + height}, t1{b1[0], b1[1], b1[2] + height};
					const P3 normal{n[0], n[1], 0};
					// (the kerb picture's plain concrete strip, its last quarter, down from the top: the rest are painted red
					// and yellow - no stopping, everywhere - as LS's kerbs)
					const float kPlain = row; // (the light grey strip: the one above it is a darker grey)
					// (a taller wall - the sidewalk's outer edge over the verge, 0.4 m - squeezed into that strip: past its end
					// the picture wraps round to the red rows, every sidewalk's back painted as LS's no-stopping kerbs)
					const float vb = kPlain + std::min(height / 0.625f, 0.185f);
					const uint32_t i0 = Add(m, b0, normal, {s / kTexture, vb}), i1 = Add(m, b1, normal, {(s + l) / kTexture, vb}),
					               i2 = Add(m, t1, normal, {(s + l) / kTexture, kPlain}), i3 = Add(m, t0, normal, {s / kTexture, kPlain});
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
			void Fan(Mesh& m, const P3& centre, const std::vector<P3>& ring, uint8_t material, bool ground, uint8_t kind = 0)
			{
				// (texture coordinates from the middle: small - research/phase0.md §61.25)
				const P3 up{0, 0, 1};
				const uint32_t c = Add(m, centre, up, {0, 0});
				std::vector<uint32_t> r;
				for (const P3& p : ring)
					r.push_back(Add(m, p, up, {(p[0] - centre[0]) / kTexture, -(p[1] - centre[1]) / kTexture}));
				for (size_t i = 0; i < ring.size(); ++i)
				{
					const size_t j = (i + 1) % ring.size();
					Tri(m, c, r[i], r[j]);
					if (material)
						Collide(centre, ring[i], ring[j], material, ground, kind);
				}
			}
			// A marking ribbon along a polyline at offset `o` (per point, left positive), `width` wide, from the atlas
			// column u0..u1 repeating every `repeat` metres along v; dashed: `dash` metres every `period`; only between
			// distances `from` and `to` along it.
			void Marking(const std::vector<P2>& p, const std::vector<float>& z, const std::vector<float>& sAlong, const std::vector<float>& o, float width,
			    float u0, float u1, float repeat, float dash = 0, float period = 0, float v0 = 0, float v1 = 1, float from = -1e30f, float to = 1e30f)
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
					const float oo = o[i] + (o[i + 1] - o[i]) * t;
					l = {q[0] + nn[0] * (oo + width / 2), q[1] + nn[1] * (oo + width / 2), qz};
					r = {q[0] + nn[0] * (oo - width / 2), q[1] + nn[1] * (oo - width / 2), qz};
				};
				const float s0 = std::max(sAlong.front(), from), s1 = std::min(sAlong.back(), to);
				if (s1 - s0 < 0.05f)
					return;
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

	namespace
	{
		constexpr std::array<uint8_t, 4> kDecalColour{255, 253, 0, 255}; // (as the markings')

		// The carriageway of an edge (research/phase0.md §61.22): rows across it at the edge's points (and closer where
		// its relief needs them: over the sinking features, all along an unpaved road), each row's columns from its right
		// edge to its left (closer over the features), zipped together; its mesh and its collision from the same
		// vertices, the collision broken (the game's pothole materials) where a pothole or a broken edge is; the
		// features' decals on the surface over them.
		void Carriageway(Builder& b, Surfaces& out, paving::Paving pv, const std::vector<paving::Feature>& features, uint32_t seed, const std::vector<P2>& p,
		    const std::vector<float>& z, const std::vector<float>& s, const std::vector<P2>& nrm, const std::vector<float>& scale,
		    const std::vector<float>& halfAt, bool worn, bool ground)
		{
			using Kind = paving::Feature::Kind;
			struct Row
			{
				float s = 0, z = 0, h = 0;
				P2 p{}, v{}; // the centre line's point, the offset per metre to the left
				std::vector<float> cols;
			};
			const float sStart = s.front(), sEnd = s.back();
			const bool rough = paving::Rough(pv);
			// the cross-section at distance si (the offset per metre interpolated as it is, so that a row between two of
			// the edge's points lies on the quad between them)
			const auto rowAt = [&](float si) {
				size_t i = 0;
				while (i + 2 < s.size() && s[i + 1] < si)
					++i;
				const float t = s[i + 1] > s[i] ? std::clamp((si - s[i]) / (s[i + 1] - s[i]), 0.0f, 1.0f) : 0.0f;
				Row r;
				r.s = si;
				r.z = z[i] + (z[i + 1] - z[i]) * t;
				r.h = halfAt[i] + (halfAt[i + 1] - halfAt[i]) * t;
				r.p = {p[i][0] + (p[i + 1][0] - p[i][0]) * t, p[i][1] + (p[i + 1][1] - p[i][1]) * t};
				const P2 v0{nrm[i][0] * scale[i], nrm[i][1] * scale[i]}, v1{nrm[i + 1][0] * scale[i + 1], nrm[i + 1][1] * scale[i + 1]};
				r.v = {v0[0] + (v1[0] - v0[0]) * t, v0[1] + (v1[1] - v0[1]) * t};
				return r;
			};
			const auto relief = [&](float si, float o, float h) { return paving::Relief(pv, features, seed, si, o, h, sStart, sEnd); };
			// the surface's normal from the relief's slope
			const auto normalAt = [&](const Row& r, float o) {
				const float e = 0.05f, lv = std::hypot(r.v[0], r.v[1]);
				const P2 left{r.v[0] / lv, r.v[1] / lv}, along{left[1], -left[0]};
				const float ds = (relief(r.s + e, o, r.h) - relief(r.s - e, o, r.h)) / (2 * e), dq = (relief(r.s, o + e, r.h) - relief(r.s, o - e, r.h)) / (2 * e * lv);
				const float gx = ds * along[0] + dq * left[0], gy = ds * along[1] + dq * left[1], l = std::sqrt(gx * gx + gy * gy + 1);
				return P3{-gx / l, -gy / l, 1 / l};
			};
			const auto sinks = [](const paving::Feature& f) { return f.kind != Kind::Patch; };
			// where the rows go (the edge's own points kept exactly)
			std::vector<std::pair<float, bool>> at;
			for (const float si : s)
				at.push_back({si, true});
			const auto fill = [&](float a, float c, float step) {
				const int n = std::max(1, static_cast<int>(std::ceil((c - a) / step)));
				for (int k = 0; k <= n; ++k)
					at.push_back({a + (c - a) * k / n, false});
			};
			// (unpaved: a row every 2 m - their bumps are longer than that, the ruts run along: the countryside's tracks at
			// 1 m were half of a rural land's collision)
			if (rough)
				fill(sStart, sEnd, 2.0f);
			for (const auto& f : features)
				if (sinks(f))
					fill(f.s0, f.s1, 0.3f);
			std::sort(at.begin(), at.end(), [](const auto& x, const auto& y) { return x.first < y.first; });
			std::vector<Row> rows;
			std::vector<bool> base;
			for (const auto& [si, own] : at)
			{
				if (!rows.empty() && si - rows.back().s < 0.02f)
				{
					if (own && !base.back())
						rows.back() = rowAt(si), base.back() = true;
					continue;
				}
				rows.push_back(rowAt(si));
				base.push_back(own);
			}
			for (Row& r : rows)
			{
				std::vector<float> c{-r.h, r.h};
				if (rough)
					for (float o = -r.h + 1.0f; o < r.h - 0.5f; o += 1.0f)
						c.push_back(o);
				for (const auto& f : features)
					if (sinks(f) && r.s >= f.s0 - 1e-3f && r.s <= f.s1 + 1e-3f)
					{
						const int n = std::max(1, static_cast<int>(std::ceil((f.o1 - f.o0) / 0.3f)));
						for (int k = 0; k <= n; ++k)
							c.push_back(std::clamp(f.o0 + (f.o1 - f.o0) * k / n, -r.h, r.h));
					}
				std::sort(c.begin(), c.end());
				r.cols.clear();
				for (const float o : c)
					if (r.cols.empty() || o - r.cols.back() > 0.02f)
						r.cols.push_back(o);
				if (r.cols.back() < r.h)
					r.cols.back() = r.h;
			}
			// (asphalt: worn roads' cracked; its picture spans kerb to kerb, a whole number of times - the game's road
			// pictures have their gutters at their edges, which met down the middle of the road otherwise)
			const bool asphalt = pv == paving::Paving::Asphalt;
			Mesh& m = asphalt && worn ? out.worn : out.paved[static_cast<size_t>(pv)];
			const float tm = paving::TextureMetres(pv);
			const float across = std::max(1.0f, std::round(2 * halfAt[halfAt.size() / 2] / tm));
			const uint8_t material = paving::Material(pv), broken = paving::BrokenMaterial(pv);
			std::vector<std::vector<uint32_t>> index(rows.size());
			std::vector<std::vector<P3>> pos(rows.size());
			for (size_t ri = 0; ri < rows.size(); ++ri)
			{
				const Row& r = rows[ri];
				for (const float o : r.cols)
				{
					const float dz = relief(r.s, o, r.h);
					const P3 q{r.p[0] + r.v[0] * o, r.p[1] + r.v[1] * o, r.z + dz};
					const P3 normal = rough || !features.empty() ? normalAt(r, o) : P3{0, 0, 1};
					index[ri].push_back(b.Add(m, q, normal, {asphalt ? (o + r.h) / (2 * r.h) * across : o / tm, r.s / tm}));
					pos[ri].push_back(q);
				}
			}
			for (size_t ri = 0; ri + 1 < rows.size(); ++ri)
			{
				const Row &ra = rows[ri], &rb = rows[ri + 1];
				const auto ta = [&](size_t k) { return (ra.cols[k] + ra.h) / (2 * ra.h); };
				const auto tb = [&](size_t k) { return (rb.cols[k] + rb.h) / (2 * rb.h); };
				size_t i = 0, j = 0;
				while (i + 1 < ra.cols.size() || j + 1 < rb.cols.size())
				{
					const bool alongB = i + 1 >= ra.cols.size() || (j + 1 < rb.cols.size() && tb(j + 1) <= ta(i + 1));
					// (anticlockwise from above: s along the road, o to its left)
					const size_t ia = i, jb = j;
					float cs, co;
					if (alongB)
					{
						b.Tri(m, index[ri][ia], index[ri + 1][jb], index[ri + 1][jb + 1]);
						cs = (ra.s + 2 * rb.s) / 3, co = (ra.cols[ia] + rb.cols[jb] + rb.cols[jb + 1]) / 3;
						b.Collide(pos[ri][ia], pos[ri + 1][jb], pos[ri + 1][jb + 1], paving::BrokenAt(features, cs, co) ? broken : material, ground, 1);
						++j;
					}
					else
					{
						b.Tri(m, index[ri][ia], index[ri + 1][jb], index[ri][ia + 1]);
						cs = (2 * ra.s + rb.s) / 3, co = (ra.cols[ia] + rb.cols[jb] + ra.cols[ia + 1]) / 3;
						b.Collide(pos[ri][ia], pos[ri + 1][jb], pos[ri][ia + 1], paving::BrokenAt(features, cs, co) ? broken : material, ground, 1);
						++i;
					}
				}
			}
			// the decals: a grid on the surface over each feature's box (the sinking ones' on the surface's own rows and
			// columns there), lifted 12 mm, its picture spread over it
			for (const auto& f : features)
			{
				Mesh& dm = f.kind == Kind::Pothole ? out.potholes : f.kind == Kind::Patch ? out.patches : out.breakups;
				float u0, v0, u1, v1;
				paving::Picture(f, u0, v0, u1, v1);
				const float step = sinks(f) ? 0.3f : 1.0f;
				const int ns = std::max(1, static_cast<int>(std::ceil((f.s1 - f.s0) / step))), no = std::max(1, static_cast<int>(std::ceil((f.o1 - f.o0) / step)));
				{
					const Row r = rowAt((f.s0 + f.s1) / 2);
					const float o = (f.o0 + f.o1) / 2;
					out.wear.push_back({f.kind, {r.p[0] + r.v[0] * o, r.p[1] + r.v[1] * o, r.z}});
				}
				const uint32_t first = static_cast<uint32_t>(dm.vertices.size());
				for (int a = 0; a <= ns; ++a)
				{
					const Row r = rowAt(f.s0 + (f.s1 - f.s0) * a / ns);
					for (int c = 0; c <= no; ++c)
					{
						const float o = f.o0 + (f.o1 - f.o0) * c / no;
						const P3 q{r.p[0] + r.v[0] * o, r.p[1] + r.v[1] * o, r.z + relief(r.s, o, r.h) + 0.012f};
						auto colour = kDecalColour;
						colour[3] = static_cast<uint8_t>(std::lround(255 * paving::Fade(f, r.s, o, r.h)));
						b.Add(dm, q, normalAt(r, o), {u0 + (u1 - u0) * c / no, v0 + (v1 - v0) * a / ns}, colour);
					}
				}
				for (int a = 0; a < ns; ++a)
					for (int c = 0; c < no; ++c)
					{
						const uint32_t q00 = first + a * (no + 1) + c, q10 = q00 + no + 1;
						b.Tri(dm, q00, q10, q10 + 1);
						b.Tri(dm, q00, q10 + 1, q00 + 1);
					}
			}
		}

		// a letter's strokes in a cell of the STOP picture (x0..x1, y0..y1 pixels; strokes 30 % of the cell's width thick
		// across, 12 % of its height along - at 22 and 9 % the letters read as thin outlines from a driver's seat)
		void Letter(texture::Image& im, char letter, int x0, int x1, int y0, int y1)
		{
			const int tx = std::max(2, (x1 - x0) * 30 / 100), ty = std::max(2, (y1 - y0) * 12 / 100), ym = (y0 + y1) / 2;
			const auto box = [&](int a0, int a1, int b0, int b1) {
				for (int y = std::max(0, b0); y < std::min(im.height, b1); ++y)
					for (int x = std::max(0, a0); x < std::min(im.width, a1); ++x)
						im.At(x, y) = {245, 245, 240, 255};
			};
			switch (letter)
			{
			case 'S':
				box(x0, x1, y0, y0 + ty), box(x0, x1, ym - ty / 2, ym + ty / 2 + 1), box(x0, x1, y1 - ty, y1);
				box(x0, x0 + tx, y0, ym), box(x1 - tx, x1, ym, y1);
				break;
			case 'T':
				box(x0, x1, y0, y0 + ty), box((x0 + x1) / 2 - tx / 2, (x0 + x1) / 2 + tx / 2 + 1, y0, y1);
				break;
			case 'O':
				box(x0, x1, y0, y0 + ty), box(x0, x1, y1 - ty, y1), box(x0, x0 + tx, y0, y1), box(x1 - tx, x1, y0, y1);
				break;
			case 'P':
				box(x0, x0 + tx, y0, y1), box(x0, x1, y0, y0 + ty), box(x0, x1, ym - ty / 2, ym + ty / 2 + 1), box(x1 - tx, x1, y0, ym);
				break;
			}
		}

		// Divided roads' medians (research/phase0.md §61.27). OpenStreetMap draws a road with a painted median or a centre
		// turn lane as two one-way carriageways a few metres apart, often with short links across between them (its turn
		// lanes and U-turns). The space between two such carriageways (one-way streets going opposite ways, nothing else
		// between them, at most kMedian apart) is paved - each its half, up to the line between them - unless the ground
		// there is mostly planted (a landscaped median); the links across a paved median are not drawn (the traffic keeps
		// them: Paths has the whole network).
		constexpr float kMedian = 7.0f;

		// a carriageway of a divided road can be: a one-way street on the ground
		bool Divides(const Edge& e) { return (e.forward > 0) != (e.backward > 0) && !Minor(e) && !e.bridge; }

		// The paved width beyond each side of an edge (left and right of a -> b), sampled every `step` metres along it.
		struct Median
		{
			float step = 2;
			std::vector<float> left, right;

			float At(bool leftSide, float s) const
			{
				const auto& w = leftSide ? left : right;
				if (w.size() < 2)
					return w.empty() ? 0.0f : w[0];
				const float x = std::clamp(s / step, 0.0f, static_cast<float>(w.size() - 1));
				const size_t i = std::min(static_cast<size_t>(x), w.size() - 2);
				const float t = x - i;
				// (no width creeping in from a sample without: a run's ends are square)
				if (w[i] <= 0 || w[i + 1] <= 0)
					return t < 0.5f ? w[i] : w[i + 1];
				return w[i] + (w[i + 1] - w[i]) * t;
			}
		};

		// The links across a divided road's median: both ends where the link meets a carriageway going on through (one
		// piece in, one out, about straight on), the carriageways at the two ends going opposite ways; at most 70 m long.
		std::vector<bool> Crossovers(const Network& n)
		{
			std::vector<bool> out(n.edges.size(), false);
			// the carriageway going through vertex v besides edge x: its travel direction there
			const auto through = [&](int v, int x, P2& dir) {
				const auto& list = n.vertices[v].edges;
				if (list.size() != 3)
					return false;
				std::vector<int> others;
				for (const int ei : list)
					if (ei != x)
						others.push_back(ei);
				if (others.size() != 2)
					return false;
				P2 t[2];
				bool into[2];
				for (int k = 0; k < 2; ++k)
				{
					const Edge& c = n.edges[others[k]];
					if (!Divides(c) || c.a == c.b)
						return false;
					const bool atB = c.b == v;
					const P2 d = atB ? Dir(c.points[c.points.size() - 2], c.points.back()) : Dir(c.points[0], c.points[1]);
					const float sign = c.forward > 0 ? 1.0f : -1.0f;
					t[k] = {d[0] * sign, d[1] * sign};
					into[k] = (c.forward > 0) == atB;
				}
				if (into[0] == into[1] || t[0][0] * t[1][0] + t[0][1] * t[1][1] < 0.8f)
					return false;
				dir = Dir({0, 0}, {t[0][0] + t[1][0], t[0][1] + t[1][1]});
				return true;
			};
			for (size_t x = 0; x < n.edges.size(); ++x)
			{
				const Edge& e = n.edges[x];
				P2 da, db;
				if (e.a == e.b || e.bridge || Length(e.points) > 70 || !through(e.a, static_cast<int>(x), da) || !through(e.b, static_cast<int>(x), db))
					continue;
				out[x] = da[0] * db[0] + da[1] * db[1] < -0.8f;
			}
			return out;
		}

		// Each carriageway's median sides (none for the other edges; `skip`: the links across, not looked at).
		std::vector<Median> Medians(const Network& n, const std::vector<bool>& skip, const std::function<bool(float, float)>& planted)
		{
			const size_t ne = n.edges.size();
			std::vector<Median> out(ne);
			// every edge's segments by 16 m cell
			constexpr float kCell = 16;
			const auto key = [](int i, int j) { return static_cast<uint64_t>(static_cast<uint32_t>(i)) << 32 | static_cast<uint32_t>(j); };
			std::unordered_map<uint64_t, std::vector<std::pair<int, int>>> cells;
			for (size_t ei = 0; ei < ne; ++ei)
			{
				if (skip[ei])
					continue;
				const auto& p = n.edges[ei].points;
				for (size_t k = 0; k + 1 < p.size(); ++k)
					for (int j = static_cast<int>(std::floor(std::min(p[k][1], p[k + 1][1]) / kCell)); j <= static_cast<int>(std::floor(std::max(p[k][1], p[k + 1][1]) / kCell)); ++j)
						for (int i = static_cast<int>(std::floor(std::min(p[k][0], p[k + 1][0]) / kCell)); i <= static_cast<int>(std::floor(std::max(p[k][0], p[k + 1][0]) / kCell));
						     ++i)
							cells[key(i, j)].push_back({static_cast<int>(ei), static_cast<int>(k)});
			}
			for (size_t ei = 0; ei < ne; ++ei)
			{
				const Edge& e = n.edges[ei];
				if (skip[ei] || !Divides(e))
					continue;
				const float total = Length(e.points), h = e.HalfWidth(), sign = e.forward > 0 ? 1.0f : -1.0f;
				Median& m = out[ei];
				const int count = std::max(2, static_cast<int>(std::ceil(total / m.step)) + 1);
				m.step = total / (count - 1);
				m.left.assign(count, 0), m.right.assign(count, 0);
				// per sample and side: the gap to the other carriageway (-1: none), whether the ground in it is planted
				std::vector<float> gap[2] = {std::vector<float>(count, -1), std::vector<float>(count, -1)};
				std::vector<bool> green[2] = {std::vector<bool>(count, false), std::vector<bool>(count, false)};
				size_t seg = 0;
				float segStart = 0;
				for (int k = 0; k < count; ++k)
				{
					const float s = k * m.step;
					while (seg + 2 < e.points.size() && segStart + Dist(e.points[seg], e.points[seg + 1]) < s)
						segStart += Dist(e.points[seg], e.points[seg + 1]), ++seg;
					const P2 &a = e.points[seg], &c = e.points[seg + 1];
					const float l = Dist(a, c), t = l > 0 ? std::clamp((s - segStart) / l, 0.0f, 1.0f) : 0.0f;
					const P2 q{a[0] + (c[0] - a[0]) * t, a[1] + (c[1] - a[1]) * t}, d = Dir(a, c), travel{d[0] * sign, d[1] * sign};
					for (int side = 0; side < 2; ++side)
					{
						const P2 u = side == 0 ? Left(d) : P2{-Left(d)[0], -Left(d)[1]};
						// the nearest road that way
						const float reach = h + kMedian + 8.5f;
						const P2 end{q[0] + u[0] * reach, q[1] + u[1] * reach};
						float best = reach;
						int hit = -1;
						P2 hitDir{};
						for (int j = static_cast<int>(std::floor(std::min(q[1], end[1]) / kCell)); j <= static_cast<int>(std::floor(std::max(q[1], end[1]) / kCell)); ++j)
							for (int i = static_cast<int>(std::floor(std::min(q[0], end[0]) / kCell)); i <= static_cast<int>(std::floor(std::max(q[0], end[0]) / kCell)); ++i)
							{
								const auto it = cells.find(key(i, j));
								if (it == cells.end())
									continue;
								for (const auto& [fi, fk] : it->second)
								{
									if (fi == static_cast<int>(ei))
										continue;
									const P2 &f0 = n.edges[fi].points[fk], &f1 = n.edges[fi].points[fk + 1];
									const P2 w{f1[0] - f0[0], f1[1] - f0[1]};
									const float den = u[0] * w[1] - u[1] * w[0];
									if (std::fabs(den) < 1e-6f)
										continue;
									const P2 aq{f0[0] - q[0], f0[1] - q[1]};
									const float tr = (aq[0] * w[1] - aq[1] * w[0]) / den, v = (aq[0] * u[1] - aq[1] * u[0]) / den;
									if (v < 0 || v > 1 || tr <= 0 || tr >= best)
										continue;
									best = tr, hit = fi, hitDir = Dir(f0, f1);
								}
							}
						if (hit < 0)
							continue;
						const Edge& o = n.edges[hit];
						const float os = o.forward > 0 ? 1.0f : -1.0f;
						if (!Divides(o) || (hitDir[0] * travel[0] + hitDir[1] * travel[1]) * os > -0.85f)
							continue; // (not the other carriageway of a divided road)
						const float g = best - h - o.HalfWidth();
						if (g > kMedian)
							continue;
						// (carriageways that touch or overlap - North Macomb Street's two, 10 m apart with three and two lanes and bike
						// lanes, 1.75 m into each other -: a paved median of 12 cm, no kerb and sidewalk on that side - else each drew
						// its inner kerb inside the other's lanes, the reviewers' "broken, twisted raised kerb pieces")
						gap[side][k] = std::max(0.12f, g);
						// (a gap under 2 m never a planted median: the aerial pictures' green there - a tree's crown over the road, the
						// verge beside - had made a strip of kerbs 1 m wide, twisting with the carriageways, down the middle of North
						// Macomb Street, the reviewers' "broken raised kerb pieces lying in the carriageway")
						if (planted && g > 2.0f)
						{
							const float mid = h + g / 2;
							green[side][k] = planted(q[0] + u[0] * mid, q[1] + u[1] * mid);
						}
					}
				}
				// the sides' runs of samples with the other carriageway beside (gaps in them up to 6 m bridged): paved but
				// where mostly planted, or shorter than 8 m
				const int bridge = static_cast<int>(std::ceil(6.0f / m.step)), shortest = static_cast<int>(std::ceil(8.0f / m.step));
				for (int side = 0; side < 2; ++side)
				{
					auto& width = side == 0 ? m.left : m.right;
					for (int k = 0; k < count;)
					{
						if (gap[side][k] < 0)
						{
							++k;
							continue;
						}
						int last = k, greenSamples = 0, samples = 0;
						for (int x = k; x < count && x - last <= bridge; ++x)
							if (gap[side][x] >= 0)
							{
								last = x, ++samples;
								greenSamples += green[side][x];
							}
						if (last - k + 1 >= shortest && greenSamples * 2 <= samples)
						{
							// (the bridged samples take their neighbours' width)
							float before = gap[side][k];
							for (int x = k; x <= last; ++x)
							{
								if (gap[side][x] >= 0)
									before = gap[side][x];
								width[x] = before / 2;
							}
						}
						k = last + 1;
					}
					// (no paving narrower than 10 cm)
					for (float& x : width)
						if (x < 0.05f)
							x = 0;
				}
			}
			return out;
		}

		// Of the links across (`crossover`), those inside the paved medians: every 2 m of them on a carriageway next to
		// its ends or on its median paving.
		std::vector<bool> Covered(const Network& n, const std::vector<bool>& crossover, const std::vector<Median>& medians)
		{
			std::vector<bool> out(n.edges.size(), false);
			for (size_t x = 0; x < n.edges.size(); ++x)
			{
				if (!crossover[x])
					continue;
				const Edge& e = n.edges[x];
				std::vector<int> beside;
				for (const int v : {e.a, e.b})
					for (const int c : n.vertices[v].edges)
						if (c != static_cast<int>(x))
							beside.push_back(c);
				const float total = Length(e.points);
				const int count = std::max(2, static_cast<int>(std::ceil(total / 2)) + 1);
				bool all = true;
				for (int k = 0; k < count && all; ++k)
				{
					// the point k / (count - 1) of the way along
					float want = total * k / (count - 1), s = 0;
					P2 q = e.points.back();
					for (size_t i = 0; i + 1 < e.points.size(); ++i)
					{
						const float l = Dist(e.points[i], e.points[i + 1]);
						if (s + l >= want)
						{
							const float t = l > 0 ? (want - s) / l : 0;
							q = {e.points[i][0] + (e.points[i + 1][0] - e.points[i][0]) * t, e.points[i][1] + (e.points[i + 1][1] - e.points[i][1]) * t};
							break;
						}
						s += l;
					}
					bool on = false;
					for (const int c : beside)
					{
						// the nearest point of the carriageway: how far, which side, how far along
						const Edge& ce = n.edges[c];
						float bestD = 1e30f, bestS = 0, along = 0;
						bool left = true;
						for (size_t i = 0; i + 1 < ce.points.size(); ++i)
						{
							const P2 &a = ce.points[i], &b2 = ce.points[i + 1];
							const float l = Dist(a, b2);
							const P2 d = Dir(a, b2);
							const float t = std::clamp(((q[0] - a[0]) * d[0] + (q[1] - a[1]) * d[1]), 0.0f, l);
							const P2 f{a[0] + d[0] * t, a[1] + d[1] * t};
							const float dist = Dist(f, q);
							if (dist < bestD)
								bestD = dist, bestS = along + t, left = d[0] * (q[1] - a[1]) - d[1] * (q[0] - a[0]) > 0;
							along += l;
						}
						if (bestD <= ce.HalfWidth() + medians[c].At(left, bestS) + 0.75f)
						{
							on = true;
							break;
						}
					}
					all = on;
				}
				out[x] = all;
			}
			return out;
		}
	}

	float CornerReach(const Network& n, int vertex)
	{
		const auto& list = n.vertices[vertex].edges;
		if (std::count_if(list.begin(), list.end(), [&](int e) { return !Minor(n.edges[e]); }) < 3)
			return 0;
		float side = 0;
		for (const int e : list)
			side = std::max(side, n.edges[e].HalfWidth() + n.edges[e].Walk());
		// (the roads stop short of a junction by its side * 1.15 + 0.5 - Geometry - and the corner's sidewalk is that far out)
		return std::hypot(side * 1.15f + 0.5f + 3, side);
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
			// (a sidewalk's width by the street: 1.6 m - five feet - on a residential street, 2.2 m on a bigger one; downtown's
			// wider, set by the build)
			e.walkWidth = r.sidewalkWidth > 0 ? r.sidewalkWidth
			            : r.kind.starts_with("primary") || r.kind.starts_with("secondary") || r.kind.starts_with("trunk") || r.kind.starts_with("tertiary")
			                ? 2.2f
			                : 1.6f;
			e.parking = r.parking;
			e.bike = r.bikeLanes ? 1.5f : 0.0f;
			e.curb = r.curb && !r.sidewalks;
			e.walkLeft = r.sidewalkLeft, e.walkRight = r.sidewalkRight;
			e.centre = r.centreLanes;
			e.forward = r.forward, e.backward = r.backward, e.laneWidth = r.laneWidth, e.sidewalks = r.sidewalks, e.bridge = r.bridge, e.layer = r.layer, e.kind = r.kind,
			e.name = r.name, e.speed = r.speed, e.id = r.id, e.surface = r.surface, e.smoothness = r.smoothness;
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
		// traffic signals: at the junction nearest each signal node (within 25 m; OpenStreetMap puts them at the junction or
		// on the roads coming in), a junction of three streets or more before one of a street and driveways
		for (const auto& p : w.points)
		{
			if (p.kind != "traffic_signals")
				continue;
			int best = -1;
			float bestD = 25;
			bool bestStreets = false;
			for (size_t v = 0; v < out.vertices.size(); ++v)
			{
				const auto& list = out.vertices[v].edges;
				if (list.size() < 3)
					continue;
				const bool streets = std::count_if(list.begin(), list.end(), [&](int ei) { return !Minor(out.edges[ei]); }) >= 3;
				const float d = Dist(out.vertices[v].p, p.position);
				if (d < 25 && ((streets && !bestStreets) || (streets == bestStreets && d < bestD)))
					bestD = d, best = static_cast<int>(v), bestStreets = streets;
			}
			if (best >= 0)
				out.vertices[best].signals = true;
		}
		// crossings and stop signs as OpenStreetMap maps them: on the edge whose junction end (within 20 m) and line (within
		// 8 m) are nearest - a crossing painted when the map says so (or where signals stop the traffic: the map's untagged
		// ones there are), a stop sign for the traffic coming in at that end
		for (const auto& p : w.points)
		{
			const bool marked = p.kind == "crossing:marked" || p.kind == "crossing", stop = p.kind == "stop";
			if (!marked && !stop)
				continue;
			int bestEdge = -1;
			bool bestAtA = false;
			float bestD = 1e30f;
			for (size_t ei = 0; ei < out.edges.size(); ++ei)
			{
				const Edge& e = out.edges[ei];
				if (Minor(e) || e.points.size() < 2)
					continue;
				for (const bool atA : {true, false})
				{
					const int v = atA ? e.a : e.b;
					if (out.vertices[static_cast<size_t>(v)].edges.size() < 3)
						continue;
					const float toJunction = Dist(out.vertices[static_cast<size_t>(v)].p, p.position);
					if (toJunction > 20)
						continue;
					// (the point's distance to the edge's first or last 25 m)
					float toLine = 1e30f;
					const size_t m = e.points.size();
					for (size_t k = 0; k + 1 < m; ++k)
					{
						const P2 &q0 = e.points[atA ? k : m - 1 - k], &q1 = e.points[atA ? k + 1 : m - 2 - k];
						const P2 d{q1[0] - q0[0], q1[1] - q0[1]};
						const float l2 = d[0] * d[0] + d[1] * d[1];
						const float t = l2 > 0 ? std::clamp(((p.position[0] - q0[0]) * d[0] + (p.position[1] - q0[1]) * d[1]) / l2, 0.0f, 1.0f) : 0.0f;
						toLine = std::min(toLine, Dist({q0[0] + d[0] * t, q0[1] + d[1] * t}, p.position));
						if (Dist(q1, out.vertices[static_cast<size_t>(v)].p) > 25)
							break;
					}
					if (toLine < 8 && toLine + toJunction * 0.2f < bestD)
						bestD = toLine + toJunction * 0.2f, bestEdge = static_cast<int>(ei), bestAtA = atA;
				}
			}
			if (bestEdge < 0)
				continue;
			Edge& e = out.edges[static_cast<size_t>(bestEdge)];
			if (stop)
				(bestAtA ? e.stopA : e.stopB) = true, ++out.mappedStops;
			else if (p.kind == "crossing:marked" || out.vertices[static_cast<size_t>(bestAtA ? e.a : e.b)].signals)
				(bestAtA ? e.crossingA : e.crossingB) = true, ++out.mappedCrossings;
		}
		// a point at least every kHeightStep m: the map's straight stretches have their points far apart (100 m and more), and
		// the road's heights - the ground's at its points - ran straight between them through the crests and over the dips
		// between (the street pictures' roads go over the hills; ours were flat to the horizon)
		for (Edge& e : out.edges)
		{
			std::vector<P2> dense{e.points.front()};
			for (size_t i = 1; i < e.points.size(); ++i)
			{
				const P2 &a = e.points[i - 1], &b = e.points[i];
				const int pieces = static_cast<int>(std::ceil(Dist(a, b) / kHeightStep));
				for (int k = 1; k < pieces; ++k)
				{
					const float t = static_cast<float>(k) / static_cast<float>(pieces);
					dense.push_back({a[0] + (b[0] - a[0]) * t, a[1] + (b[1] - a[1]) * t});
				}
				dense.push_back(b);
			}
			e.points = std::move(dense);
		}
		return out;
	}

	void Heights(Network& n, const std::function<float(float, float)>& ground, float clearance)
	{
		// the vertices: in the air where only bridges meet (at their highest layer, 1 at least, over the ground or the sea),
		// else on the ground
		for (Vertex& v : n.vertices)
		{
			bool air = !v.edges.empty();
			int layer = 1;
			for (const int ei : v.edges)
				air &= n.edges[ei].bridge, layer = std::max(layer, n.edges[ei].layer);
			const float g = ground(v.p[0], v.p[1]);
			v.z = air ? std::max(g, 0.0f) + kLevel * layer : std::max(g + 0.05f, 0.6f);
		}
		for (Edge& e : n.edges)
		{
			e.z.resize(e.points.size());
			const float total = Length(e.points);
			const float za = n.vertices[e.a].z, zb = n.vertices[e.b].z;
			// (a bridge between two ends on the ground mostly over water: arched, the sea's way under it)
			bool arch = false;
			if (e.bridge)
			{
				const auto air = [&](int v) {
					return std::all_of(n.vertices[v].edges.begin(), n.vertices[v].edges.end(), [&](int ei) { return n.edges[ei].bridge; });
				};
				size_t wet = 0;
				for (const P2& p : e.points)
					wet += ground(p[0], p[1]) < 0;
				arch = !air(e.a) && !air(e.b) && wet * 2 > e.points.size();
			}
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
				const float t = total > 0 ? s / total : 0;
				const float rise = arch ? std::min(clearance, total * 0.08f) * std::sin(kPi * t) : 0.0f;
				e.z[i] = std::max(za + (zb - za) * t + rise, g + 1.0f);
			}
			e.z.front() = za;
			e.z.back() = zb;
		}
		// (a vertex on the ground: at its edges' mean end height, as before)
		for (Vertex& v : n.vertices)
		{
			if (std::all_of(v.edges.begin(), v.edges.end(), [&](int ei) { return n.edges[ei].bridge; }) && !v.edges.empty())
				continue;
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
			v.z = count ? sum / count : v.z;
		}
		for (Edge& e : n.edges)
		{
			e.z.front() = n.vertices[e.a].z;
			e.z.back() = n.vertices[e.b].z;
		}
	}

	texture::Image BikePicture()
	{
		// (256 across, 512 along: the arrow in the top 40 %, the bicycle below - its wheels' rings, a frame of the hubs, the
		// crank, the saddle and the bars - white everywhere, so the cut-out's edges stay white as it shrinks)
		texture::Image im(256, 512, {245, 245, 240, 0});
		const auto put = [&](float x, float y) {
			const int xi = static_cast<int>(x), yi = static_cast<int>(y);
			if (xi >= 0 && xi < im.width && yi >= 0 && yi < im.height)
				im.At(xi, yi) = {245, 245, 240, 255};
		};
		// (a stroke from a to b, 9 px across and 13 along - round about as thick both ways once stretched)
		const auto stroke = [&](float ax, float ay, float bx, float by) {
			const int steps = static_cast<int>(std::max(std::fabs(bx - ax), std::fabs(by - ay))) + 1;
			for (int k = 0; k <= steps; ++k)
			{
				const float x = ax + (bx - ax) * k / steps, y = ay + (by - ay) * k / steps;
				for (int dy = -6; dy <= 6; ++dy)
					for (int dx = -4; dx <= 4; ++dx)
						put(x + dx, y + dy);
			}
		};
		const auto ring = [&](float cx, float cy, float rx, float ry) {
			for (int a = 0; a < 360; a += 2)
			{
				const float t = a * 3.14159265f / 180;
				const float x = cx + std::cos(t) * rx, y = cy + std::sin(t) * ry;
				for (int dy = -6; dy <= 6; ++dy)
					for (int dx = -4; dx <= 4; ++dx)
						put(x + dx, y + dy);
			}
		};
		// the arrow: its shaft and head (tip at the top)
		for (int y = 6; y < 70; ++y)
		{
			const float half = (y - 6) * 0.9f;
			for (int x = static_cast<int>(128 - half); x <= static_cast<int>(128 + half); ++x)
				put(static_cast<float>(x), static_cast<float>(y));
		}
		for (int y = 70; y < 200; ++y)
			for (int x = 114; x <= 142; ++x)
				put(static_cast<float>(x), static_cast<float>(y));
		// the bicycle: wheels at 25 % and 75 % across, 88 % down
		const float rearX = 64, frontX = 192, hubY = 430, rx = 50, ry = 72;
		ring(rearX, hubY, rx, ry);
		ring(frontX, hubY, rx, ry);
		const float crankX = 118, crankY = 430, seatX = 104, seatY = 300, barX = 176, barY = 296;
		stroke(rearX, hubY, crankX, crankY);
		stroke(rearX, hubY, seatX, seatY);
		stroke(crankX, crankY, seatX, seatY);
		stroke(seatX, seatY, barX, barY + 14);
		stroke(crankX, crankY, barX, barY + 14);
		stroke(barX, barY, frontX, hubY);
		stroke(seatX - 22, seatY - 8, seatX + 18, seatY - 8);
		stroke(barX - 6, barY - 10, barX + 22, barY - 18);
		return im;
	}

	texture::Image StopPicture()
	{
		// (512 across, 512 along: four cells across with gaps, the letters filling their height; white everywhere, so that
		// the cut-out's edges stay white as it shrinks)
		texture::Image im(512, 512, {245, 245, 240, 0});
		const int cell = 512 / 4, gap = 14;
		const char letters[] = {'S', 'T', 'O', 'P'};
		for (int k = 0; k < 4; ++k)
			Letter(im, letters[k], k * cell + gap / 2, (k + 1) * cell - gap / 2, 10, 502);
		return im;
	}

	void Geometry(const Network& source, Surfaces& out, const std::function<bool(float, float)>& planted, const std::function<float(float, float)>& ground)
	{
		Builder b{out};
		// 0. divided roads' medians: paved where narrow and not planted, the links across those not drawn (gone from the
		// vertices' lists: the carriageways go on through there)
		const std::vector<bool> crossover = Crossovers(source);
		const std::vector<Median> medians = Medians(source, crossover, planted);
		const std::vector<bool> hidden = Covered(source, crossover, medians);
		Network n = source;
		for (Vertex& v : n.vertices)
			std::erase_if(v.edges, [&](int ei) { return hidden[ei]; });
		const size_t nv = n.vertices.size(), ne = n.edges.size();
		// 1. what each vertex is: a junction (three roads or more, or a road meeting itself), a change of road (two
		// roads not alike: the wider narrows to the other), a street going past driveways or alleys (two streets and minor
		// roads: the street goes on through, its sidewalks too, as in a real street; the minor roads end at the
		// sidewalk's outer edge, without crossings, stop lines or signals), or a dead end
		enum class Kind : uint8_t { None, End, Change, Driveway, Junction };
		std::vector<Kind> kind(nv, Kind::None);
		std::vector<float> reach(nv, 0);
		std::vector<int> streets(nv, 0); // roads at the vertex that are not minor
		for (size_t v = 0; v < nv; ++v)
		{
			const auto& list = n.vertices[v].edges;
			kind[v] = list.size() >= 3 || (list.size() == 2 && list[0] == list[1]) ? Kind::Junction : list.size() == 2 ? Kind::Change : list.size() == 1 ? Kind::End
			                                                                                                                                              : Kind::None;
			std::vector<int> through;
			for (const int ei : list)
				if (!Minor(n.edges[ei]))
					through.push_back(ei);
			streets[v] = static_cast<int>(through.size());
			if (kind[v] == Kind::Junction && list.size() >= 3 && through.size() == 2 && through[0] != through[1])
				kind[v] = Kind::Driveway;
			for (const int ei : list)
				reach[v] = std::max(reach[v], n.edges[ei].HalfWidth() + n.edges[ei].Walk());
		}
		// (an edge's direction leaving vertex v)
		const auto away = [&](const Edge& e, int v) {
			return e.a == v ? Dir(e.points[0], e.points[1]) : Dir(e.points.back(), e.points[e.points.size() - 2]);
		};
		// 2. junctions joined by a road too short to show between them are one junction (offset crossings, the two
		// carriageways of a divided road)
		std::vector<int> cluster(nv);
		for (size_t v = 0; v < nv; ++v)
			cluster[v] = static_cast<int>(v);
		const auto root = [&](int v) {
			while (cluster[v] != v)
				v = cluster[v] = cluster[cluster[v]];
			return v;
		};
		std::vector<bool> inside(ne, false);
		for (size_t ei = 0; ei < ne; ++ei)
		{
			const Edge& e = n.edges[ei];
			if (e.a == e.b || kind[e.a] != Kind::Junction || kind[e.b] != Kind::Junction)
				continue;
			if (Length(e.points) < (reach[e.a] + reach[e.b]) * 1.25f + 2)
			{
				inside[ei] = true;
				cluster[root(e.a)] = root(e.b);
			}
		}
		for (size_t ei = 0; ei < ne; ++ei)
			if (hidden[ei])
				inside[ei] = true; // (not drawn: a link across a paved median)
		std::vector<P3> centre(nv, P3{0, 0, 0});
		std::vector<int> members(nv, 0);
		for (size_t v = 0; v < nv; ++v)
		{
			if (kind[v] != Kind::Junction)
				continue;
			const int r = root(static_cast<int>(v));
			centre[r][0] += n.vertices[v].p[0], centre[r][1] += n.vertices[v].p[1], centre[r][2] += n.vertices[v].z;
			++members[r];
		}
		for (size_t v = 0; v < nv; ++v)
			if (members[v])
				centre[v] = {centre[v][0] / members[v], centre[v][1] / members[v], centre[v][2] / members[v]};
		// 3. how far each edge stops short of its junctions (changes of road: not at all)
		std::vector<float> trimA(ne, 0), trimB(ne, 0);
		for (size_t ei = 0; ei < ne; ++ei)
		{
			const Edge& e = n.edges[ei];
			if (inside[ei])
				continue;
			const float total = Length(e.points);
			for (const bool atA : {true, false})
			{
				const int v = atA ? e.a : e.b;
				if (kind[v] == Kind::Driveway && Minor(e))
				{
					// a driveway or alley: to the outer edge of the street's sidewalk (at its angle to the street)
					float half = 0, walk = 0;
					P2 along{1, 0};
					for (const int s : n.vertices[v].edges)
						if (!Minor(n.edges[s]))
						{
							half = std::max(half, n.edges[s].HalfWidth());
							walk = std::max(walk, n.edges[s].Walk());
							along = away(n.edges[s], v);
						}
					const P2 d = away(e, v);
					const float sine = std::fabs(d[0] * along[1] - d[1] * along[0]);
					(atA ? trimA : trimB)[ei] = std::min((half + walk) / std::max(0.4f, sine) + 0.05f, 0.45f * total);
					continue;
				}
				if (kind[v] != Kind::Junction)
					continue;
				// (from a merged junction's middle: past the whole of it)
				const P3& c = centre[root(v)];
				const float extra = std::max(0.0f, Dist(n.vertices[v].p, {c[0], c[1]}) * 0.5f);
				(atA ? trimA : trimB)[ei] = std::min(reach[v] * 1.15f + 0.5f + extra, 0.45f * total);
			}
		}
		// the road a change of road leads into, as seen going along `ei` (its lanes in this edge's direction)
		struct Next
		{
			bool any = false;
			float half = 0;
			int lanesF = 0, lanesB = 0, centre = 0;
			float laneWidth = 3.5f, parking = 0;
			bool sidewalks = false;
			int other = -1;
		};
		const auto nextAt = [&](size_t ei, bool atA) {
			Next x;
			const Edge& e = n.edges[ei];
			const int v = atA ? e.a : e.b;
			if (kind[v] != Kind::Change && (kind[v] != Kind::Driveway || Minor(e)))
				return x;
			const auto& list = n.vertices[v].edges;
			int other = -1;
			if (kind[v] == Kind::Change)
				other = list[0] == static_cast<int>(ei) ? list[1] : list[0];
			else
				for (const int s : list)
					if (s != static_cast<int>(ei) && !Minor(n.edges[s]))
						other = s; // (the street going on past the driveway)
			if (other < 0)
				return x;
			const Edge& o = n.edges[other];
			x.any = true;
			x.other = other;
			x.half = o.HalfWidth();
			x.laneWidth = o.laneWidth;
			x.parking = o.Side();
			x.sidewalks = o.sidewalks;
			x.centre = o.centre;
			// traffic going our way: at our start it comes out of the other; at our end it goes into it
			const bool otherPointsHere = o.b == v;
			if (atA)
				x.lanesF = otherPointsHere ? o.forward : o.backward, x.lanesB = otherPointsHere ? o.backward : o.forward;
			else
				x.lanesF = otherPointsHere ? o.backward : o.forward, x.lanesB = otherPointsHere ? o.forward : o.backward;
			return x;
		};
		// edge ends at junctions: the cross-section where the edge stops (outward: away from the junction)
		struct End
		{
			int edge;
			bool atA;
			P2 at, outward;
			float half;         // the road's half width
			float walkL, walkR; // its sidewalks' widths (0: none), left and right of outward
			float z;
			bool crossing;
			float fillL = 0, fillR = 0; // median paving beyond its edges
		};
		std::vector<std::vector<End>> ends(nv);
		for (size_t ei = 0; ei < ne; ++ei)
		{
			const Edge& e = n.edges[ei];
			if (inside[ei])
				continue;
			const float total = Length(e.points);
			std::vector<P2> p0;
			std::vector<float> z0, s0;
			Cut(e.points, e.z, trimA[ei], total - trimB[ei], p0, z0, s0);
			if (p0.size() < 2)
				continue;
			const Next na = nextAt(ei, true), nb = nextAt(ei, false);
			const float half = e.HalfWidth(), w = e.laneWidth;
			// this road's layout: the centre line's offset (left positive) with its lanes, as the next road has it
			// (a parking lane on the right before the lanes)
			const auto centreOf = [](float h, int lanesF, float lw, float park) { return -h + 0.3f + park + lanesF * lw; };
			// tapers: where this road changes into a narrower one, its last metres narrow to it and its centre line
			// moves over to the next road's (of two alike wide, the first in the list moves); a sidewalk the next road
			// has not narrows to nothing
			const auto owns = [&](const Next& x) { return x.half < half - 1e-3f || (std::fabs(x.half - half) <= 1e-3f && static_cast<int>(ei) < x.other); };
			const auto taperOf = [&](const Next& x) {
				if (!x.any)
					return 0.0f;
				const float shift = owns(x) ? std::max(half - x.half, std::fabs(centreOf(half, e.forward, w, e.Side()) - centreOf(x.half, x.lanesF, x.laneWidth, x.parking)))
				                            : 0.0f;
				const float walk = e.sidewalks && !x.sidewalks ? 10.0f : 0.0f;
				return shift > 0.05f || walk > 0 ? std::clamp(shift * 12.0f + walk, 10.0f, 40.0f) : 0.0f;
			};
			const float sStart = s0.front(), sEnd = s0.back();
			const float tapA = std::min(taperOf(na), 0.45f * (sEnd - sStart)), tapB = std::min(taperOf(nb), 0.45f * (sEnd - sStart));
			// (points every 2 m in the tapers, so that their edges curve smoothly)
			std::vector<P2> p;
			std::vector<float> z, s;
			for (size_t i = 0; i < p0.size(); ++i)
			{
				if (i)
				{
					const float a0 = s0[i - 1], a1 = s0[i];
					const bool tapering = (tapA > 0 && a0 < sStart + tapA) || (tapB > 0 && a1 > sEnd - tapB);
					const int steps = tapering ? std::max(1, static_cast<int>(std::ceil((a1 - a0) / 2.0f))) : 1;
					for (int k = 1; k < steps; ++k)
					{
						const float t = static_cast<float>(k) / steps;
						p.push_back({p0[i - 1][0] + (p0[i][0] - p0[i - 1][0]) * t, p0[i - 1][1] + (p0[i][1] - p0[i - 1][1]) * t});
						z.push_back(z0[i - 1] + (z0[i] - z0[i - 1]) * t);
						s.push_back(a0 + (a1 - a0) * t);
					}
				}
				p.push_back(p0[i]), z.push_back(z0[i]), s.push_back(s0[i]);
			}
			// how far into a taper each point is: 1 at the change of road, 0 where the taper starts
			const auto into = [&](float si, bool atA) {
				const float tap = atA ? tapA : tapB;
				if (tap <= 0)
					return 0.0f;
				const float d = atA ? si - sStart : sEnd - si;
				const float t = std::clamp(1 - d / tap, 0.0f, 1.0f);
				return t * t * (3 - 2 * t);
			};
			// (centreAt: the left edge of the lanes going along; a centre turn lane, turnAt wide, between it and the lanes against)
			std::vector<float> halfAt(p.size()), walkAt(p.size()), walkAtL(p.size()), walkAtR(p.size()), centreAt(p.size()), parkAt(p.size()), turnAt(p.size());
			for (size_t i = 0; i < p.size(); ++i)
			{
				const float ta = into(s[i], true), tb = into(s[i], false);
				float h = half, c = centreOf(half, e.forward, w, e.Side()), wk = e.Walk() > 0 ? e.Walk() : e.curb ? kCurbTop : 0.0f, pk = e.Side();
				float turn = e.centre * w;
				// (a side without its sidewalk: the kerb and gutter)
				float wl = e.sidewalks && !e.walkLeft ? kCurbTop : wk, wr = e.sidewalks && !e.walkRight ? kCurbTop : wk;
				for (const auto& [t, x] : {std::pair{ta, &na}, std::pair{tb, &nb}})
				{
					if (t <= 0)
						continue;
					// (the narrower of the two keeps its width: only the wider narrows)
					if (x->half < half)
						h = h + (x->half - half) * t, pk = pk + (x->parking - e.Side()) * t;
					if (owns(*x))
						c = c + (centreOf(x->half, x->lanesF, x->laneWidth, x->parking) - centreOf(half, e.forward, w, e.Side())) * t;
					if ((e.sidewalks || e.curb) && !x->sidewalks)
						wk = wk * (1 - t), wl = wl * (1 - t), wr = wr * (1 - t);
					turn = turn + (x->centre * x->laneWidth - e.centre * w) * t;
				}
				halfAt[i] = h, walkAt[i] = std::max(wl, wr), walkAtL[i] = wl, walkAtR[i] = wr, centreAt[i] = c, parkAt[i] = pk, turnAt[i] = turn;
			}
			// the median paving beyond each side at each point (and so no sidewalk there)
			const Median& md = medians[ei];
			std::vector<float> fillL(p.size()), fillR(p.size());
			for (size_t i = 0; i < p.size(); ++i)
				fillL[i] = md.At(true, s[i]), fillR[i] = md.At(false, s[i]);
			std::vector<std::pair<float, float>> medianL, medianR; // (where it is paved: from, to along the edge)
			std::vector<float> scale;
			auto nrm = Normals(p, scale);
			// at a change of road the two meet on the bisector of their directions (no wedge between them)
			for (const bool atA : {true, false})
			{
				const Next& x = atA ? na : nb;
				if (!x.any)
					continue;
				const Edge& o = n.edges[x.other];
				const int v = atA ? e.a : e.b;
				const size_t last = o.points.size() - 1;
				P2 d0, d1;
				if (atA)
					d0 = o.b == v ? Dir(o.points[last - 1], o.points[last]) : Dir(o.points[1], o.points[0]), d1 = Dir(p[0], p[1]);
				else
					d0 = Dir(p[p.size() - 2], p.back()), d1 = o.a == v ? Dir(o.points[0], o.points[1]) : Dir(o.points[last], o.points[last - 1]);
				P2 m{d0[0] + d1[0], d0[1] + d1[1]};
				const float lm = std::hypot(m[0], m[1]);
				if (lm < 1e-3f)
					continue;
				m = {m[0] / lm, m[1] / lm};
				const size_t i = atA ? 0 : p.size() - 1;
				const P2 own = Left(atA ? d1 : d0);
				nrm[i] = Left(m);
				scale[i] = 1.0f / std::max(0.5f, nrm[i][0] * own[0] + nrm[i][1] * own[1]);
			}
			std::vector<P3> right, left, walkRi, walkRo, walkLi, walkLo;
			std::vector<std::array<float, 2>> uvWi, uvWoL, uvWoR;
			for (size_t i = 0; i < p.size(); ++i)
			{
				const float k = scale[i], h = halfAt[i];
				const auto off = [&](float o, float dz) { return P3{p[i][0] + nrm[i][0] * o * k, p[i][1] + nrm[i][1] * o * k, z[i] + dz}; };
				right.push_back(off(-h, 0));
				left.push_back(off(h, 0));
				walkRi.push_back(off(-h, kKerb));
				walkRo.push_back(off(-h - walkAtR[i], kKerb));
				walkLi.push_back(off(h, kKerb));
				walkLo.push_back(off(h + walkAtL[i], kKerb));
				uvWi.push_back({0, s[i] / kTexture});
				uvWoL.push_back({walkAtL[i] / kTexture, s[i] / kTexture});
				uvWoR.push_back({walkAtR[i] / kTexture, s[i] / kTexture});
			}
			// the carriageway: what it is made of (OSM surface), its wear (potholes, patches, broken edges: paving.hpp) clear
			// of its painted lines, its crossings and stop lines and its tapers
			const paving::Paving pv = paving::Of(e.surface);
			const bool crossA = kind[e.a] == Kind::Junction, crossB = kind[e.b] == Kind::Junction;
			const float clearA = crossA ? 4.6f : 0, clearB = crossB ? 4.6f : 0;
			// (as in the US: a double yellow line between the directions of the through roads - residential streets,
			// service roads and alleys have none; one-way roads a yellow line on their left edge, white on the right)
			const bool through = !(e.kind == "residential" || e.kind == "service" || e.kind == "living_street" || e.kind == "unclassified" || e.kind == "road" ||
			                       e.kind == "track");
			const bool major = e.kind.starts_with("motorway") || e.kind.starts_with("trunk") || e.kind.starts_with("primary") || e.kind.starts_with("secondary");
			{
				std::vector<float> lines;
				const float c0 = centreOf(half, e.forward, w, e.Side()), t0 = e.centre * w;
				if (e.forward && e.backward && e.centre)
					lines.insert(lines.end(), {c0 + 0.08f, c0 + 0.32f, c0 + t0 - 0.08f, c0 + t0 - 0.32f});
				else if (e.forward && e.backward && through)
					lines.insert(lines.end(), {c0 - 0.12f, c0 + 0.12f});
				for (int j = 1; j < e.forward; ++j)
					lines.push_back(c0 - j * w);
				for (int j = 1; j < e.backward; ++j)
					lines.push_back(c0 + t0 + j * w);
				if (major || e.bike > 0)
					lines.insert(lines.end(), {-half + 0.45f + e.Side(), half - 0.45f - e.Side()});
				const uint32_t seed = static_cast<uint32_t>(e.id) * 2654435761u ^ static_cast<uint32_t>(std::lround(p.front()[0] * 10)) * 40503u ^
				                      static_cast<uint32_t>(std::lround(p.front()[1] * 10));
				const float f0 = sStart + std::max({clearA + 1.0f, tapA, 1.5f}), f1 = sEnd - std::max({clearB + 1.0f, tapB, 1.5f});
				const float wear = paving::Wear(e.kind, e.smoothness, e.id);
				const auto features = f1 - f0 > 3 ? paving::Features(pv, wear, seed, f0, f1, half, lines, major) : std::vector<paving::Feature>{};
				Carriageway(b, out, pv, features, seed, p, z, s, nrm, scale, halfAt, wear >= 0.5f, !e.bridge);
				// the median paving: from the carriageway's edge out to the line between the carriageways (rows at its points
				// and every 2 m between them), the same surface, its picture from the middle of the lanes'
				if (!md.left.empty())
				{
					Mesh& mm = pv == paving::Paving::Asphalt && wear >= 0.5f ? out.worn : out.paved[static_cast<size_t>(pv)];
					const float tm = paving::TextureMetres(pv);
					std::vector<float> rs;
					for (size_t i = 0; i < p.size(); ++i)
					{
						rs.push_back(s[i]);
						if (i + 1 < p.size())
							for (float x = std::floor(s[i] / 2) * 2 + 2; x < s[i + 1] - 0.2f; x += 2)
								if (x > s[i] + 0.2f)
									rs.push_back(x);
					}
					for (const bool leftSide : {true, false})
					{
						std::vector<float> paved(rs.size());
						for (size_t r = 0; r < rs.size(); ++r)
							paved[r] = md.At(leftSide, rs[r]);
						for (size_t r = 0; r + 1 < rs.size();)
						{
							if (paved[r] <= 0 && paved[r + 1] <= 0)
							{
								++r;
								continue;
							}
							size_t last = r + 1;
							while (last + 1 < rs.size() && (paved[last] > 0 || paved[last + 1] > 0))
								++last;
							std::vector<P3> inner, outer;
							std::vector<std::array<float, 2>> uvI, uvO;
							for (size_t k = r; k <= last; ++k)
							{
								size_t i = 0;
								while (i + 2 < s.size() && s[i + 1] < rs[k])
									++i;
								const float t = s[i + 1] > s[i] ? std::clamp((rs[k] - s[i]) / (s[i + 1] - s[i]), 0.0f, 1.0f) : 0.0f;
								const P2 c{p[i][0] + (p[i + 1][0] - p[i][0]) * t, p[i][1] + (p[i + 1][1] - p[i][1]) * t};
								const P2 v{nrm[i][0] * scale[i] + (nrm[i + 1][0] * scale[i + 1] - nrm[i][0] * scale[i]) * t,
								    nrm[i][1] * scale[i] + (nrm[i + 1][1] * scale[i + 1] - nrm[i][1] * scale[i]) * t};
								const float h = halfAt[i] + (halfAt[i + 1] - halfAt[i]) * t, zz = z[i] + (z[i + 1] - z[i]) * t;
								const float sign = leftSide ? 1.0f : -1.0f, o0 = sign * h, o1 = sign * (h + paved[k]);
								inner.push_back({c[0] + v[0] * o0, c[1] + v[1] * o0, zz});
								outer.push_back({c[0] + v[0] * o1, c[1] + v[1] * o1, zz});
								uvI.push_back({0.4f, rs[k] / tm});
								uvO.push_back({0.6f, rs[k] / tm});
							}
							if (leftSide)
								b.Strip(mm, inner, outer, uvI, uvO, paving::Material(pv), true, 1);
							else
								b.Strip(mm, outer, inner, uvO, uvI, paving::Material(pv), true, 1);
							(leftSide ? medianL : medianR).push_back({rs[r], rs[last]});
							r = last;
						}
					}
				}
			}
			if (e.sidewalks || e.curb)
			{
				// (each side along its stretches without median paving; a kerb and gutter alone: a narrow kerb top)
				const auto stretches = [&](const std::vector<float>& fill, auto&& f) {
					for (size_t i = 0; i < p.size();)
					{
						if (fill[i] > 0)
						{
							++i;
							continue;
						}
						size_t j = i;
						while (j + 1 < p.size() && fill[j + 1] <= 0)
							++j;
						if (j > i)
							f(i, j + 1);
						i = j + 1;
					}
				};
				const auto slice = [](const auto& v, size_t i0, size_t i1) { return std::decay_t<decltype(v)>(v.begin() + i0, v.begin() + i1); };
				// the kerb's face: plain concrete (the kerb picture's plain row; its yellow paint, put at every corner by a rule,
				// was where the street pictures have none - which kerbs are painted the map does not say)
				constexpr float kPlainRow = 0.81f;
				const auto kerb = [&](const std::vector<P3>& line, size_t i0, size_t i1, bool faceLeft) {
					b.Wall(out.kerb, slice(line, i0, i1), kKerb, faceLeft, kConcrete, kPlainRow);
				};
				stretches(fillL, [&](size_t i0, size_t i1) {
					b.Strip(out.sidewalk, slice(walkLi, i0, i1), slice(walkLo, i0, i1), slice(uvWi, i0, i1), slice(uvWoL, i0, i1), kPavement, !e.bridge,
					    2);                       // (left: inner on the right of its direction)
					kerb(left, i0, i1, false); // facing the road (to the right of the left kerb's direction)
					// the sidewalk's outer edge down to the ground below it
					auto lo = slice(walkLo, i0, i1);
					for (P3& q : lo)
						q[2] -= kKerb + 0.25f;
					b.Wall(out.kerb, lo, kKerb + 0.25f, true, 0);
				});
				stretches(fillR, [&](size_t i0, size_t i1) {
					b.Strip(out.sidewalk, slice(walkRo, i0, i1), slice(walkRi, i0, i1), slice(uvWoR, i0, i1), slice(uvWi, i0, i1), kPavement, !e.bridge, 2);
					kerb(right, i0, i1, true);
					auto ro = slice(walkRo, i0, i1);
					for (P3& q : ro)
						q[2] -= kKerb + 0.25f;
					b.Wall(out.kerb, ro, kKerb + 0.25f, false, 0);
				});
			}
			// a bridge's structure (concrete): its parapets past the sidewalks or the carriageway's edges (0.4 m wide, 0.95 m
			// high, walls the cars keep off), the deck's sides and underside 1.2 m down, and a pier every 30 m where the ground
			// is more than 2.5 m below - one column, two and a cap across under a wide deck
			if (e.bridge)
			{
				constexpr float kParapet = 0.95f, kDeck = 1.2f;
				std::vector<P3> innerL, innerR, outerL, outerR;
				for (size_t i = 0; i < p.size(); ++i)
				{
					const float k = scale[i], edge = halfAt[i] + walkAt[i];
					const auto off = [&](float o, float dz) { return P3{p[i][0] + nrm[i][0] * o * k, p[i][1] + nrm[i][1] * o * k, z[i] + dz}; };
					innerL.push_back(off(edge, walkAt[i] > 0 ? kKerb : 0.0f));
					innerR.push_back(off(-edge, walkAt[i] > 0 ? kKerb : 0.0f));
					outerL.push_back(off(edge + 0.4f, -kDeck));
					outerR.push_back(off(-edge - 0.4f, -kDeck));
				}
				const auto raised = [](std::vector<P3> v, float dz) {
					for (P3& q : v)
						q[2] += dz;
					return v;
				};
				const auto across = [](const std::vector<P3>& v, float dx) { return v; };
				(void)across;
				// parapets: the inner face (collision), the top, the outer face down to the deck's underside
				b.Wall(out.structure, innerL, kParapet, false, kConcrete);
				b.Wall(out.structure, innerR, kParapet, true, kConcrete);
				std::vector<P3> topL, topR, topLo, topRo;
				for (size_t i = 0; i < p.size(); ++i)
				{
					topL.push_back({innerL[i][0], innerL[i][1], innerL[i][2] + kParapet});
					topR.push_back({innerR[i][0], innerR[i][1], innerR[i][2] + kParapet});
					topLo.push_back({outerL[i][0], outerL[i][1], innerL[i][2] + kParapet});
					topRo.push_back({outerR[i][0], outerR[i][1], innerR[i][2] + kParapet});
				}
				std::vector<std::array<float, 2>> uv0(p.size()), uv1(p.size());
				for (size_t i = 0; i < p.size(); ++i)
					uv0[i] = {0, s[i] / 7.4f}, uv1[i] = {0.05f, s[i] / 7.4f};
				b.Strip(out.structure, topL, topLo, uv0, uv1, kConcrete, false);
				b.Strip(out.structure, topRo, topR, uv1, uv0, kConcrete, false);
				b.Wall(out.structure, outerL, kDeck + kParapet + (walkAt[0] > 0 ? kKerb : 0.0f), true, 0);
				b.Wall(out.structure, outerR, kDeck + kParapet + (walkAt[0] > 0 ? kKerb : 0.0f), false, 0);
				// the underside (facing down: the strip from the left edge to the right)
				{
					std::vector<std::array<float, 2>> uvL(p.size()), uvR(p.size());
					for (size_t i = 0; i < p.size(); ++i)
					{
						const float wdt = Dist({outerL[i][0], outerL[i][1]}, {outerR[i][0], outerR[i][1]});
						uvL[i] = {0, s[i] / 7.4f}, uvR[i] = {wdt / 7.4f, s[i] / 7.4f};
					}
					b.Strip(out.structure, outerL, outerR, uvL, uvR, kConcrete, false);
					// (its normals down)
					for (size_t v = out.structure.vertices.size() - (p.size() - 1) * 4; v < out.structure.vertices.size(); ++v)
						out.structure.vertices[v].normal = {0, 0, -1};
				}
				// piers
				if (ground)
				{
					const float length = s.back() - s.front();
					for (float at = s.front() + std::fmod(length, 30.0f) / 2 + (length >= 30 ? 0.0f : length / 2); at < s.back() - 4; at += 30)
					{
						if (at < s.front() + 4)
							continue;
						size_t i = 0;
						while (i + 2 < s.size() && s[i + 1] < at)
							++i;
						const float t = s[i + 1] > s[i] ? std::clamp((at - s[i]) / (s[i + 1] - s[i]), 0.0f, 1.0f) : 0.0f;
						const P2 c{p[i][0] + (p[i + 1][0] - p[i][0]) * t, p[i][1] + (p[i + 1][1] - p[i][1]) * t};
						const float top = z[i] + (z[i + 1] - z[i]) * t - kDeck;
						const float g = ground(c[0], c[1]);
						if (top - g < 2.5f)
							continue;
						const P2 d = Dir(p[i], p[i + 1]), l = Left(d);
						const float width = Dist({outerL[i][0], outerL[i][1]}, {outerR[i][0], outerR[i][1]});
						// a box from the ground (half a metre into it) up to `h`, `a` along the road and `w` across, at offset o
						const auto box = [&](float o, float a, float wdt, float bottom, float h) {
							const P2 m{c[0] + l[0] * o, c[1] + l[1] * o};
							P3 corner[4];
							for (int k = 0; k < 4; ++k)
							{
								const float sa = (k == 1 || k == 2) ? 0.5f : -0.5f, sw = k >= 2 ? 0.5f : -0.5f;
								corner[k] = {m[0] + d[0] * a * sa + l[0] * wdt * sw, m[1] + d[1] * a * sa + l[1] * wdt * sw, bottom};
							}
							// (the four sides, anticlockwise round: 0 back-right, 1 front-right, 2 front-left, 3 back-left)
							for (int k = 0; k < 4; ++k)
							{
								const P3 a0 = corner[k], a1 = corner[(k + 1) % 4];
								b.Wall(out.structure, {a0, a1}, h - bottom, false, kConcrete);
							}
						};
						if (width < 13)
							box(0, 1.6f, 1.6f, g - 0.5f, top);
						else
						{
							for (const float o : {-width / 4, width / 4})
								box(o, 1.6f, 1.6f, g - 0.5f, top - 1.0f);
							box(0, 1.8f, width - 1.0f, top - 1.0f, top); // (the cap across)
						}
					}
				}
			}
			// markings (on asphalt and concrete): between the directions (double yellow), between lanes (dashed white: a
			// lane the next road has not ends where the taper starts), edge lines on the big roads; the stretch before a
			// junction keeps a crossing (3 m) and the stop line behind it clear
			const bool painted = paving::Marked(pv);
			const auto offsets = [&](auto&& f) {
				std::vector<float> o(p.size());
				for (size_t i = 0; i < p.size(); ++i)
					o[i] = f(i);
				return o;
			};
			const auto line = [&](const std::vector<float>& o, float width, float u0, float u1, float repeat, float from, float to, float dash = 0,
			                      float period = 0, float v0 = 0, float v1 = 1) {
				if (painted && to - from > 0.5f)
					b.Marking(p, z, s, o, width, u0, u1, repeat, dash, period, v0, v1, from, to);
			};
			if (e.forward && e.backward && e.centre)
			{
				// (a two-way left turn lane: each edge a solid yellow line by the lanes going on, a dashed one inside it)
				line(offsets([&](size_t i) { return centreAt[i] + 0.08f; }), 0.15f, kYellowU0, kYellowU1, 4.0f, sStart + clearA, sEnd - clearB);
				line(offsets([&](size_t i) { return centreAt[i] + 0.32f; }), 0.15f, kYellowU0, kYellowU1, 4.0f, sStart + clearA, sEnd - clearB, 3.0f, 12.0f, 0.15f, 0.85f);
				line(offsets([&](size_t i) { return centreAt[i] + turnAt[i] - 0.08f; }), 0.15f, kYellowU0, kYellowU1, 4.0f, sStart + clearA, sEnd - clearB);
				line(offsets([&](size_t i) { return centreAt[i] + turnAt[i] - 0.32f; }), 0.15f, kYellowU0, kYellowU1, 4.0f, sStart + clearA, sEnd - clearB, 3.0f,
				    12.0f, 0.15f, 0.85f);
			}
			else if (e.forward && e.backward && through)
			{
				line(offsets([&](size_t i) { return centreAt[i] + 0.12f; }), 0.16f, kYellowU0, kYellowU1, 4.0f, sStart + clearA, sEnd - clearB);
				line(offsets([&](size_t i) { return centreAt[i] - 0.12f; }), 0.16f, kYellowU0, kYellowU1, 4.0f, sStart + clearA, sEnd - clearB);
			}
			for (int j = 1; j < e.forward; ++j)
			{
				const float from = sStart + std::max(clearA, na.any && j >= na.lanesF ? tapA : 0.0f);
				const float to = sEnd - std::max(clearB, nb.any && j >= nb.lanesF ? tapB : 0.0f);
				line(offsets([&](size_t i) { return centreAt[i] - j * w; }), 0.2f, kDashU0, kDashU1, 4.0f, from, to, 3.0f, 12.0f,
				    0.15f, 0.85f);
			}
			for (int j = 1; j < e.backward; ++j)
			{
				const float from = sStart + std::max(clearA, na.any && j >= na.lanesB ? tapA : 0.0f);
				const float to = sEnd - std::max(clearB, nb.any && j >= nb.lanesB ? tapB : 0.0f);
				line(offsets([&](size_t i) { return centreAt[i] + turnAt[i] + j * w; }), 0.2f, kDashU0, kDashU1, 4.0f, from, to, 3.0f, 12.0f, 0.15f, 0.85f);
			}
			if (major || e.bike > 0)
			{
				// (the right edge of the lanes going along: white; the left edge of a one-way road: yellow; inside the parking
				// and bike lanes - a bike lane's line on any road)
				const bool oneWay = !e.forward || !e.backward;
				line(offsets([&](size_t i) { return -halfAt[i] + 0.45f + parkAt[i]; }), 0.2f, oneWay && !e.forward ? kYellowU0 : kWhiteU0, oneWay && !e.forward ? kYellowU1 : kWhiteU1,
				    4.0f, sStart + clearA, sEnd - clearB);
				line(offsets([&](size_t i) { return halfAt[i] - 0.45f - parkAt[i]; }), 0.2f, oneWay && !e.backward ? kYellowU0 : kWhiteU0, oneWay && !e.backward ? kYellowU1 : kWhiteU1,
				    4.0f, sStart + clearA, sEnd - clearB);
			}
			if (!major)
			{
				// (a paved median's edges: yellow - the big roads have their edge lines already)
				for (const auto& [from, to] : medianL)
					line(offsets([&](size_t i) { return halfAt[i] - 0.3f; }), 0.16f, kYellowU0, kYellowU1, 4.0f, std::max(from, sStart + clearA), std::min(to, sEnd - clearB));
				for (const auto& [from, to] : medianR)
					line(offsets([&](size_t i) { return -halfAt[i] + 0.3f; }), 0.16f, kYellowU0, kYellowU1, 4.0f, std::max(from, sStart + clearA), std::min(to, sEnd - clearB));
			}
			// at a junction: the crossing (bars along the road, 3 m long, across its width) and the stop line of the
			// lanes coming in behind it
			const auto stopLine = [&](bool atB) {
				const size_t i = atB ? p.size() - 1 : 0;
				const int lanes = atB ? e.forward : e.backward;
				if (!lanes)
					return;
				const float sl = atB ? sEnd - 4.2f : sStart + 4.2f;
				// (the stop line across the lanes coming in: the right of the centre going to b, the left coming to a)
				const float c = centreAt[i], turn = turnAt[i];
				const float o0 = atB ? c - lanes * w : c + turn, o1 = atB ? c : c + turn + lanes * w;
				P2 q{}, d{};
				float qz = z[i];
				// the point at sl and the road's direction there
				for (size_t k = 0; k + 1 < p.size(); ++k)
					if (s[k + 1] >= sl || k + 2 == p.size())
					{
						const float t = std::clamp((sl - s[k]) / std::max(1e-3f, s[k + 1] - s[k]), 0.0f, 1.0f);
						q = {p[k][0] + (p[k + 1][0] - p[k][0]) * t, p[k][1] + (p[k + 1][1] - p[k][1]) * t};
						d = Dir(p[k], p[k + 1]);
						qz = z[k] + (z[k + 1] - z[k]) * t;
						break;
					}
				const P2 l = Left(d);
				std::vector<P2> stopLine{{q[0] + l[0] * o0 - d[0] * 0.25f, q[1] + l[1] * o0 - d[1] * 0.25f}, {q[0] + l[0] * o1 - d[0] * 0.25f, q[1] + l[1] * o1 - d[1] * 0.25f}};
				std::vector<float> lz{qz, qz}, ls{0, o1 - o0};
				if (painted)
					b.Marking(stopLine, lz, ls, std::vector<float>(2, 0.0f), 0.5f, kStripeU0, kStripeU1, 2.0f);
			};
			// (a STOP legend in each lane coming in to a stop sign, 1.8 m before its stop line, 2.4 m long, 85 % of the lane
			// wide - Tallahassee's stop-sign approaches have them painted; the reviewers missed them on four views)
			// a picture painted on the road: from `top` (away from whoever reads it) to `bottom` along it, `hw` either side of
			// the offset `o`, read going to b (`atB`) or to a; lifted 14 mm
			const auto decal = [&](Mesh& m, float top, float bottom, float o, float hw, bool atB) {
				const auto pointAt = [&](float sv, P2& q, P2& d, float& qz) {
					for (size_t k = 0; k + 1 < p.size(); ++k)
						if (s[k + 1] >= sv || k + 2 == p.size())
						{
							const float t = std::clamp((sv - s[k]) / std::max(1e-3f, s[k + 1] - s[k]), 0.0f, 1.0f);
							q = {p[k][0] + (p[k + 1][0] - p[k][0]) * t, p[k][1] + (p[k + 1][1] - p[k][1]) * t};
							d = Dir(p[k], p[k + 1]);
							qz = z[k] + (z[k + 1] - z[k]) * t;
							return;
						}
				};
				P2 qt{}, dt{}, qb{}, db{};
				float zt = 0, zb = 0;
				pointAt(top, qt, dt, zt);
				pointAt(bottom, qb, db, zb);
				// (the reader's right: right of a -> b going to b, left of it coming to a)
				const P2 lt = Left(dt), lb = Left(db);
				const float side = atB ? -1.0f : 1.0f;
				const auto at = [&](const P2& q, const P2& l, float zq, float across) {
					return P3{q[0] + l[0] * (o + across * side), q[1] + l[1] * (o + across * side), zq + 0.014f};
				};
				const uint32_t tl = b.Add(m, at(qt, lt, zt, -hw), {0, 0, 1}, {0, 0}, kDecalColour);
				const uint32_t tr = b.Add(m, at(qt, lt, zt, hw), {0, 0, 1}, {1, 0}, kDecalColour);
				const uint32_t br = b.Add(m, at(qb, lb, zb, hw), {0, 0, 1}, {1, 1}, kDecalColour);
				const uint32_t bl = b.Add(m, at(qb, lb, zb, -hw), {0, 0, 1}, {0, 1}, kDecalColour);
				// (anticlockwise from above whichever way the reader comes)
				const P3 &a3 = m.vertices[tl].position, &b3 = m.vertices[bl].position, &c3 = m.vertices[br].position;
				if ((b3[0] - a3[0]) * (c3[1] - a3[1]) - (b3[1] - a3[1]) * (c3[0] - a3[0]) > 0)
					b.Tri(m, tl, bl, br), b.Tri(m, tl, br, tr);
				else
					b.Tri(m, tl, br, bl), b.Tri(m, tl, tr, br);
			};
			const auto legend = [&](bool atB) {
				const int lanes = atB ? e.forward : e.backward;
				const float top = atB ? sEnd - 6.0f : sStart + 6.0f, bottom = atB ? sEnd - 8.4f : sStart + 8.4f;
				if (!lanes || !painted || std::min(top, bottom) < sStart + 1 || std::max(top, bottom) > sEnd - 1)
					return;
				const size_t i = atB ? p.size() - 1 : 0;
				const float c = centreAt[i], turn = turnAt[i];
				for (int j = 0; j < lanes; ++j)
					decal(out.legends, top, bottom, atB ? c - (j + 0.5f) * w : c + turn + (j + 0.5f) * w, 0.425f * w, atB);
			};
			// (a bike lane's symbol - a bicycle under an arrow, 0.9 m wide and 3 m long - 12 m past each junction it leaves and
			// every 150 m after; the bike lanes between the parking and the traffic lanes, the right of each way)
			if (e.bike > 0 && painted)
				for (const bool atB : {true, false})
				{
					if (!(atB ? e.forward : e.backward))
						continue;
					const float from = sStart + clearA + 12, to = sEnd - clearB - 12;
					for (float at = from; at + 3 <= to; at += 150)
					{
						const float sv = atB ? at : sEnd - (at - sStart);
						const size_t i = std::min(p.size() - 1, static_cast<size_t>(std::lower_bound(s.begin(), s.end(), sv) - s.begin()));
						const float o = atB ? -halfAt[i] + 0.45f + parkAt[i] + e.bike / 2 : halfAt[i] - 0.45f - parkAt[i] - e.bike / 2;
						decal(out.bikeSymbols, atB ? sv + 3 : sv - 3, sv, o, 0.45f, atB);
					}
				}
			const auto crossing = [&](bool atB) {
				const size_t i = atB ? p.size() - 1 : 0;
				const float h = halfAt[i];
				// (Florida's continental bars: 0.8 m wide, 0.8 m apart - the street pictures' are wider than the 0.55 m ones had been)
				for (float o = -h + 0.8f; o <= h - 0.7f; o += 1.6f)
				{
					const std::vector<float> oo(p.size(), o);
					line(oo, 0.8f, kStripeU0, kStripeU1, 2.0f, atB ? sEnd - 3.2f : sStart + 0.2f, atB ? sEnd - 0.2f : sStart + 3.2f);
				}
				stopLine(atB);
			};
			// (crossings where people walk: streets with sidewalks, at junctions of three streets or more - not where a
			// street meets only driveways, alleys or parking lanes)
			// (painted crossings where the map has them - a junction with signals, else where it says they are painted; the
			// street pictures' stop-sign corners mostly have none -; where the map has no crossings at all, at the signals'
			// junctions)
			const bool walked = e.sidewalks && !Minor(e);
			const bool mapped = n.mappedCrossings > 0;
			if (crossB && streets[e.b] >= 3 && (mapped ? e.crossingB : walked && n.vertices[static_cast<size_t>(e.b)].signals))
				crossing(true);
			else if (crossB && e.stopB && e.forward)
				stopLine(true), legend(true);
			if (crossA && streets[e.a] >= 3 && (mapped ? e.crossingA : walked && n.vertices[static_cast<size_t>(e.a)].signals))
				crossing(false);
			else if (crossA && e.stopA && e.backward)
				stopLine(false), legend(false);
			// this edge's ends at junctions
			for (const bool atA : {true, false})
			{
				const int v = atA ? e.a : e.b;
				if (kind[v] != Kind::Junction)
					continue;
				const size_t i = atA ? 0 : p.size() - 1, j = atA ? 1 : p.size() - 2;
				// (outward: along the edge at a - its left the edge's; against it at b)
				const float fl = atA ? fillL[i] : fillR[i], fr = atA ? fillR[i] : fillL[i];
				End x{static_cast<int>(ei), atA, p[i], Dir(p[i], p[j]), halfAt[i], fl > 0 ? 0 : atA ? walkAtL[i] : walkAtR[i], fr > 0 ? 0 : atA ? walkAtR[i] : walkAtL[i],
				    z[i], true};
				x.fillL = fl, x.fillR = fr;
				ends[root(v)].push_back(x);
			}
		}
		// 4. junctions: the asphalt inside the ends' cross-sections and the kerbs' curves between neighbouring ends
		// (each a quadratic curve through where the two road edges would meet, tangent to both); the sidewalks round
		// the corners the same way (narrowing to nothing towards a road without), straight across the side of a T
		const auto meet = [](const P2& a, const P2& da, const P2& c, const P2& dc, P2& out) {
			// a + da * t = c + dc * u
			const float den = da[0] * dc[1] - da[1] * dc[0];
			if (std::fabs(den) < 1e-3f)
				return false;
			const float t = ((c[0] - a[0]) * dc[1] - (c[1] - a[1]) * dc[0]) / den;
			const float u = ((c[0] - a[0]) * da[1] - (c[1] - a[1]) * da[0]) / den;
			if (t < 0 || u < 0)
				return false;
			out = {a[0] + da[0] * t, a[1] + da[1] * t};
			return true;
		};
		const auto curve = [&](const P2& a, const P2& da, const P2& c, const P2& dc, int steps) {
			// from a (heading da) to c (arriving against dc)
			std::vector<P2> pts;
			P2 ctrl{(a[0] + c[0]) / 2, (a[1] + c[1]) / 2};
			P2 m;
			// (where a's line going on meets c's line coming back)
			if (meet(a, da, c, {-dc[0], -dc[1]}, m) && Dist(m, ctrl) < 2.5f * Dist(a, c) + 1)
				ctrl = m;
			for (int k = 0; k <= steps; ++k)
			{
				const float t = static_cast<float>(k) / steps, u = 1 - t;
				pts.push_back({u * u * a[0] + 2 * u * t * ctrl[0] + t * t * c[0], u * u * a[1] + 2 * u * t * ctrl[1] + t * t * c[1]});
			}
			return pts;
		};
		for (size_t v = 0; v < nv; ++v)
		{
			auto& list = ends[v];
			if (list.size() < 2)
				continue;
			const P3& c = centre[v];
			const auto angleOf = [&](const End& x) {
				const P2 q{x.at[0] + x.outward[0] * 2 - c[0], x.at[1] + x.outward[1] * 2 - c[1]};
				return std::atan2(q[1], q[0]);
			};
			std::sort(list.begin(), list.end(), [&](const End& x, const End& y) { return angleOf(x) < angleOf(y); });
			std::vector<P2> ring;
			std::vector<float> ringZ;
			struct Corner
			{
				std::vector<P2> kerb, outer;
				float zx, zy;
			};
			std::vector<Corner> corners;
			for (size_t k = 0; k < list.size(); ++k)
			{
				const End& x = list[k];
				const End& y = list[(k + 1) % list.size()];
				const P2 lx = Left(x.outward), ly = Left(y.outward);
				// (the edges with any median paving beyond)
				const float xR = x.half + x.fillR, xL = x.half + x.fillL, yR = y.half + y.fillR;
				const P2 xr{x.at[0] - lx[0] * xR, x.at[1] - lx[1] * xR}, xl{x.at[0] + lx[0] * xL, x.at[1] + lx[1] * xL};
				const P2 yr{y.at[0] - ly[0] * yR, y.at[1] - ly[1] * yR};
				ring.push_back(xr), ringZ.push_back(x.z);
				ring.push_back(xl), ringZ.push_back(x.z);
				float gap = angleOf(y) - angleOf(x);
				if (gap <= 0)
					gap += 2 * kPi;
				const bool straight = gap > kPi * 0.94f;
				// the kerb from x's left road edge round to y's right one (into the junction along x, out along y)
				const P2 in{-x.outward[0], -x.outward[1]};
				const auto kerb = straight ? std::vector<P2>{xl, yr} : curve(xl, in, yr, y.outward, 8);
				for (size_t q = 1; q + 1 < kerb.size(); ++q)
					ring.push_back(kerb[q]), ringZ.push_back(x.z + (y.z - x.z) * static_cast<float>(q) / (kerb.size() - 1));
				if (x.walkL <= 0 && y.walkR <= 0)
					continue;
				const P2 xo{xl[0] + lx[0] * x.walkL, xl[1] + lx[1] * x.walkL}, yo{yr[0] - ly[0] * y.walkR, yr[1] - ly[1] * y.walkR};
				Corner cr;
				cr.kerb = kerb;
				cr.outer = straight ? std::vector<P2>{xo, yo} : curve(xo, in, yo, y.outward, 8);
				if (cr.outer.size() != cr.kerb.size())
				{
					// (a straight side: as many points on both)
					cr.kerb = {xl, yr};
					cr.outer = {xo, yo};
				}
				cr.zx = x.z, cr.zy = y.z;
				corners.push_back(std::move(cr));
			}
			// the carriageway: the ring's triangles (heights along it), world-space texture; made of what all the roads
			// meeting there are made of, else asphalt
			{
				paving::Paving jp = paving::Of(n.edges[list[0].edge].surface);
				for (const End& x : list)
					if (paving::Of(n.edges[x.edge].surface) != jp)
						jp = paving::Paving::Asphalt;
				Mesh& jm = out.paved[static_cast<size_t>(jp)];
				const float tm = paving::TextureMetres(jp);
				const uint8_t jmat = paving::Material(jp);
				std::vector<polygon::P2> poly(ring.begin(), ring.end());
				auto tris = polygon::Triangulate(poly);
				if (tris.empty())
				{
					// (a ring that crosses itself: a fan from the middle)
					std::vector<P3> r3;
					for (size_t q = 0; q < ring.size(); ++q)
						r3.push_back({ring[q][0], ring[q][1], ringZ[q]});
					b.Fan(jm, c, r3, jmat, true, 1);
				}
				else
				{
					const P3 up{0, 0, 1};
					std::vector<uint32_t> idx;
					for (size_t q = 0; q < ring.size(); ++q)
						idx.push_back(b.Add(jm, {ring[q][0], ring[q][1], ringZ[q]}, up, {(ring[q][0] - c[0]) / tm, -(ring[q][1] - c[1]) / tm}));
					for (const auto& t : tris)
					{
						b.Tri(jm, idx[t[0]], idx[t[1]], idx[t[2]]);
						b.Collide({ring[t[0]][0], ring[t[0]][1], ringZ[t[0]]}, {ring[t[1]][0], ring[t[1]][1], ringZ[t[1]]}, {ring[t[2]][0], ring[t[2]][1], ringZ[t[2]]},
						    jmat, true, 1);
					}
				}
			}
			for (const Corner& cr : corners)
			{
				std::vector<P3> inner, outer;
				std::vector<std::array<float, 2>> uvI, uvO;
				float run = 0;
				for (size_t q = 0; q < cr.kerb.size(); ++q)
				{
					const float t = static_cast<float>(q) / (cr.kerb.size() - 1);
					const float zz = cr.zx + (cr.zy - cr.zx) * t;
					if (q)
						run += Dist(cr.kerb[q - 1], cr.kerb[q]);
					inner.push_back({cr.kerb[q][0], cr.kerb[q][1], zz + kKerb});
					outer.push_back({cr.outer[q][0], cr.outer[q][1], zz + kKerb});
					uvI.push_back({0, run / kTexture});
					uvO.push_back({Dist(cr.kerb[q], cr.outer[q]) / kTexture, run / kTexture});
				}
				// (the kerb runs anticlockwise round the junction: the junction on its left, the sidewalk on its right)
				b.Strip(out.sidewalk, outer, inner, uvO, uvI, kPavement, true, 2);
				std::vector<P3> foot;
				for (size_t q = 0; q < cr.kerb.size(); ++q)
					foot.push_back({cr.kerb[q][0], cr.kerb[q][1], inner[q][2] - kKerb});
				b.Wall(out.kerb, foot, kKerb, true, kConcrete);
				std::vector<P3> outerFoot = outer;
				for (P3& q : outerFoot)
					q[2] -= kKerb + 0.25f;
				b.Wall(out.kerb, outerFoot, kKerb + 0.25f, false, 0);
			}
		}
		// 5. traffic signals at the signalled junctions: for each road coming in, a mast at the right corner of the far side
		// (as in the US: the heads hang over the far side of the junction, in the drivers' view from the stop line), its arm
		// across the lanes; a road with nothing straight across (the stem of a T) has it by its own stop line. Never on the
		// asphalt or where turning cars cut the corner (moved back over the sidewalk, or none), one to a corner; the roads
		// inside a merged junction (between the carriageways of a divided road) have none.
		{
			std::vector<bool> signalled(nv, false);
			for (size_t v = 0; v < nv; ++v)
				if (n.vertices[v].signals && kind[v] == Kind::Junction)
					signalled[root(static_cast<int>(v))] = true;
			const Asphalt asphalt(out.collision, kCornerPostClear);
			for (size_t v = 0; v < nv; ++v)
			{
				if (!signalled[v] || ends[v].size() < 3)
					continue;
				std::vector<P2> placed;
				for (const End& x : ends[v])
				{
					const Edge& e = n.edges[x.edge];
					const int lanesIn = x.atA ? e.backward : e.forward;
					if (!lanesIn || Minor(e))
						continue;
					const P2 d{-x.outward[0], -x.outward[1]}, right{d[1], -d[0]};
					// the road straight across (within about 35 degrees) that traffic leaves by (of a divided road's two
					// carriageways, the one going away: the other's right side is the median)
					const End* y = nullptr;
					float best = 0.82f;
					for (const End& o : ends[v])
					{
						const Edge& oe = n.edges[o.edge];
						if (&o != &x && (o.atA ? oe.forward : oe.backward) > 0)
							if (const float c = o.outward[0] * d[0] + o.outward[1] * d[1]; c > best)
								best = c, y = &o;
					}
					// on its sidewalk a little in from the kerb, just past its crossing; or by this road's stop line
					const End& at = y ? *y : x;
					// (the side: the far road's right of outward, this road's left)
					const float walk = y ? at.walkR : at.walkL, fill = y ? at.fillR : at.fillL;
					const float along = y ? 1.0f : 4.6f, off = at.half + fill + (walk > 0 ? std::min(0.9f, walk * 0.35f) : 1.0f);
					P2 q{at.at[0] + at.outward[0] * along + right[0] * off, at.at[1] + at.outward[1] * along + right[1] * off};
					// (back from the kerb as far as the corner's clearance asks, up to 4 m: cars turning in cut across it)
					float back = 0;
					for (; back < 4 && asphalt.Within(q[0], q[1], kCornerPostClear); back += 0.25f)
						q = {q[0] + right[0] * 0.25f, q[1] + right[1] * 0.25f};
					if (asphalt.Within(q[0], q[1], kCornerPostClear) || std::any_of(placed.begin(), placed.end(), [&](const P2& p) { return Dist(p, q) < 3; }))
						continue;
					placed.push_back(q);
					// (over the parking lane by the kerb too)
					const float park = n.edges[static_cast<size_t>(at.edge)].Side();
					out.signals.push_back({{q[0], q[1], at.z + kKerb}, std::atan2(-d[0], d[1]), lanesIn * e.laneWidth + off + back - at.half + park});
				}
			}
		}
	}

	void Rails(const std::vector<world::Rail>& rails, const std::function<float(float, float)>& ground, const std::function<bool(float, float)>& onRoad,
	    Surfaces& out)
	{
		Builder b{out};
		constexpr float kBed = 0.35f, kTrack = 2.56f, kShoulder = 1.3f, kGauge = 1.435f, kRailWidth = 0.07f, kRailHeight = 0.15f;
		constexpr float kBeam = 3.4f, kDepth = 1.8f, kParapet = 0.6f;
		constexpr uint8_t kGravel = 31, kConcreteRow = 1;
		// (the lines' ends: in the air where only bridges meet - their highest layer -, else on the ground)
		std::map<std::pair<int, int>, std::pair<bool, int>> ends; // (end, rounded to 10 cm) -> (only bridges, highest layer)
		const auto keyOf = [](const P2& p) { return std::make_pair(static_cast<int>(std::lround(p[0] * 10)), static_cast<int>(std::lround(p[1] * 10))); };
		for (const auto& r : rails)
			for (const P2* e : {&r.points.front(), &r.points.back()})
			{
				auto [it, fresh] = ends.try_emplace(keyOf(*e), std::make_pair(true, 1));
				it->second.first &= r.bridge;
				it->second.second = std::max(it->second.second, r.layer);
			}
		for (const auto& r : rails)
		{
			const auto& p = r.points;
			std::vector<float> s(p.size(), 0), g(p.size()), z(p.size());
			for (size_t i = 0; i < p.size(); ++i)
			{
				if (i)
					s[i] = s[i - 1] + Dist(p[i - 1], p[i]);
				g[i] = ground(p[i][0], p[i][1]);
			}
			const float total = s.back();
			if (total < 1)
				continue;
			// heights: on the ground the bed's top; a bridge between its ends' heights, at least 1 m over the ground
			const auto endZ = [&](const P2& e, float ge) {
				const auto& [air, layer] = ends.at(keyOf(e));
				return air && r.bridge ? std::max(ge, 0.0f) + kRailLevel * std::max(1, layer) : ge + kBed;
			};
			const float za = endZ(p.front(), g.front()), zb = endZ(p.back(), g.back());
			for (size_t i = 0; i < p.size(); ++i)
				z[i] = r.bridge ? std::max(za + (zb - za) * s[i] / total, g[i] + 1.0f) : g[i] + kBed;
			// (the track's sides: the normals at the points, mitred)
			std::vector<float> scale;
			const auto nrm = Normals(p, scale);
			const auto off = [&](size_t i, float o, float dz) { return P3{p[i][0] + nrm[i][0] * o * scale[i], p[i][1] + nrm[i][1] * o * scale[i], z[i] + dz}; };
			// stretches not on a road (the road's surface over a level crossing)
			std::vector<bool> keep(p.size() - 1, true);
			if (!r.bridge && onRoad)
				for (size_t i = 0; i + 1 < p.size(); ++i)
				{
					const P2 m{(p[i][0] + p[i + 1][0]) / 2, (p[i][1] + p[i + 1][1]) / 2};
					keep[i] = !onRoad(m[0], m[1]) && !onRoad(p[i][0], p[i][1]) && !onRoad(p[i + 1][0], p[i + 1][1]);
				}
			for (size_t i = 0; i + 1 < p.size(); ++i)
			{
				if (!keep[i])
					continue;
				const std::vector<size_t> at{i, i + 1};
				const auto line = [&](float o, float dz) {
					std::vector<P3> v;
					for (const size_t k : at)
						v.push_back(off(k, o, dz));
					return v;
				};
				const auto uvs = [&](float u) {
					std::vector<std::array<float, 2>> v;
					for (const size_t k : at)
						v.push_back({u, s[k] / kTrack});
					return v;
				};
				// the track: the picture across its 2.56 m (the rails at 22 and 78 %), every 2.56 m along
				b.Strip(out.railTrack, line(-kTrack / 2, 0), line(kTrack / 2, 0), uvs(0), uvs(1), kGravel, false);
				// the rails: a head 7 cm wide, 15 cm up, its two sides and top
				for (const float c : {-kGauge / 2, kGauge / 2})
				{
					const auto lo0 = line(c - kRailWidth / 2, 0), lo1 = line(c + kRailWidth / 2, 0);
					b.Wall(out.railSteel, lo0, kRailHeight, false, 0);
					b.Wall(out.railSteel, lo1, kRailHeight, true, 0);
					b.Strip(out.railSteel, line(c - kRailWidth / 2, kRailHeight), line(c + kRailWidth / 2, kRailHeight), uvs(0), uvs(0.1f), 0, false);
				}
				if (!r.bridge)
				{
					// the shoulders: from the track's edges down to the ground 1.3 m out
					std::vector<P3> outL, outR;
					for (const size_t k : at)
					{
						P3 l = off(k, kTrack / 2 + kShoulder, 0), q = off(k, -kTrack / 2 - kShoulder, 0);
						l[2] = ground(l[0], l[1]) - 0.05f, q[2] = ground(q[0], q[1]) - 0.05f;
						outL.push_back(l), outR.push_back(q);
					}
					std::vector<std::array<float, 2>> u0, u1;
					for (const size_t k : at)
						u0.push_back({0, s[k] / 3}), u1.push_back({kShoulder / 3, s[k] / 3});
					b.Strip(out.railBed, line(kTrack / 2, 0), outL, u0, u1, kGravel, false);
					b.Strip(out.railBed, outR, line(-kTrack / 2, 0), u1, u0, kGravel, false);
				}
				else
				{
					// the guideway: its top beside the track, parapets, sides and underside 1.8 m down
					std::vector<std::array<float, 2>> u0, u1;
					for (const size_t k : at)
						u0.push_back({0, s[k] / 7.4f}), u1.push_back({0.12f, s[k] / 7.4f});
					b.Strip(out.structure, line(kTrack / 2, 0), line(kBeam / 2, 0), u0, u1, kConcreteRow, false);
					b.Strip(out.structure, line(-kBeam / 2, 0), line(-kTrack / 2, 0), u1, u0, kConcreteRow, false);
					b.Wall(out.structure, line(kBeam / 2 - 0.25f, 0), kParapet, false, kConcreteRow);
					b.Wall(out.structure, line(-kBeam / 2 + 0.25f, 0), kParapet, true, kConcreteRow);
					b.Wall(out.structure, line(kBeam / 2, -kDepth), kDepth + kParapet, true, 0);
					b.Wall(out.structure, line(-kBeam / 2, -kDepth), kDepth + kParapet, false, 0);
					const size_t first = out.structure.vertices.size();
					b.Strip(out.structure, line(kBeam / 2, -kDepth), line(-kBeam / 2, -kDepth), u1, u0, 0, false);
					for (size_t v = first; v < out.structure.vertices.size(); ++v)
						out.structure.vertices[v].normal = {0, 0, -1};
				}
			}
			// an elevated line's piers: every 25 m where the ground is more than 2.5 m below its beam
			if (r.bridge)
				for (float at = std::fmod(total, 25.0f) / 2; at < total; at += 25)
				{
					size_t i = 0;
					while (i + 2 < s.size() && s[i + 1] < at)
						++i;
					const float t = s[i + 1] > s[i] ? std::clamp((at - s[i]) / (s[i + 1] - s[i]), 0.0f, 1.0f) : 0.0f;
					const P2 c{p[i][0] + (p[i + 1][0] - p[i][0]) * t, p[i][1] + (p[i + 1][1] - p[i][1]) * t};
					const float top = z[i] + (z[i + 1] - z[i]) * t - kDepth, gc = ground(c[0], c[1]);
					if (top - gc < 2.5f)
						continue;
					const P2 d = Dir(p[i], p[i + 1]), l = Left(d);
					P3 corner[4];
					for (int k = 0; k < 4; ++k)
					{
						const float sa = (k == 1 || k == 2) ? 0.7f : -0.7f, sw = k >= 2 ? 0.7f : -0.7f;
						corner[k] = {c[0] + d[0] * sa + l[0] * sw, c[1] + d[1] * sa + l[1] * sw, gc - 0.5f};
					}
					for (int k = 0; k < 4; ++k)
						b.Wall(out.structure, {corner[k], corner[(k + 1) % 4]}, top - gc + 0.5f, false, kConcreteRow);
				}
		}
	}

	size_t Walkways(const std::vector<world::Walk>& walks, const std::vector<std::vector<bool>>& skip, const std::function<float(float, float)>& ground,
	    const std::function<bool(float, float)>& onRoad, Surfaces& out)
	{
		Builder b{out};
		size_t made = 0;
		for (size_t wi = 0; wi < walks.size(); ++wi)
		{
			const world::Walk& wk = walks[wi];
			// (the crossings are the roads'; footbridges and the like left out - a strip on the ground under one is wrong;
			// a "grass" path is the lawn)
			if (wk.kind == "crossing" || wk.bridge || wk.tunnel || wk.layer != 0 || wk.points.size() < 2 || wk.surface == "grass")
				continue;
			// (untagged or just "paved": a sidewalk's, a footway's, a driveway's concrete - Tallahassee's map has 6700 sidewalks
			// "paved", drawn as asphalt they were the reviewers' "dark asphalt sidewalks" -, a cycleway's or a path's asphalt)
			const bool unpavedPath = (wk.kind == "path" || wk.kind == "bridleway") && wk.surface.empty();
			const bool plain = wk.surface.empty() || wk.surface == "paved";
			const paving::Paving pv = unpavedPath ? paving::Paving::Dirt
			                        : plain       ? (wk.kind == "cycleway" || wk.kind == "path" ? paving::Paving::Asphalt : paving::Paving::Concrete)
			                                      : paving::Of(wk.surface);
			const bool unpaved = pv == paving::Paving::Gravel || pv == paving::Paving::Dirt || pv == paving::Paving::Sand;
			const float width = wk.width > 0 ? std::clamp(wk.width, 0.8f, 6.0f)
			                    : wk.kind == "driveway"                       ? 3.0f
			                    : wk.kind == "sidewalk" || wk.kind == "steps" ? 1.5f
			                    : wk.kind == "cycleway"                       ? 2.4f
			                    : wk.kind == "pedestrian"                     ? 3.0f
			                    : unpaved                                     ? 1.1f
			                    : wk.kind == "path"                           ? 2.0f
			                                                                  : 1.8f;
			Mesh& mesh = pv == paving::Paving::Concrete ? out.sidewalk : out.paved[static_cast<size_t>(pv)];
			const uint8_t material = pv == paving::Paving::Concrete ? kPavement : paving::Material(pv);
			const float tm = pv == paving::Paving::Concrete ? kTexture : paving::TextureMetres(pv);
			// the line a point every metre (the ground's relief under it), its stretches over the roads and the skipped
			// pieces (a street's sidewalk the street draws itself) left out
			std::vector<P2> p;
			std::vector<bool> keep;
			for (size_t k = 0; k + 1 < wk.points.size(); ++k)
			{
				const P2 &a = wk.points[k], &c = wk.points[k + 1];
				const bool skipped = wi < skip.size() && k < skip[wi].size() && skip[wi][k];
				const int pieces = std::max(1, static_cast<int>(std::ceil(Dist(a, c))));
				for (int q = 0; q < pieces; ++q)
				{
					const float t = static_cast<float>(q) / static_cast<float>(pieces);
					p.push_back({a[0] + (c[0] - a[0]) * t, a[1] + (c[1] - a[1]) * t});
					keep.push_back(!skipped);
				}
			}
			p.push_back(wk.points.back());
			keep.push_back(keep.back());
			std::vector<float> s(p.size(), 0.0f);
			for (size_t i = 1; i < p.size(); ++i)
				s[i] = s[i - 1] + Dist(p[i - 1], p[i]);
			if (s.back() < 1)
				continue;
			std::vector<float> scale;
			const auto nrm = Normals(p, scale);
			std::vector<P3> left(p.size()), right(p.size());
			for (size_t i = 0; i < p.size(); ++i)
			{
				const float o = width / 2 * scale[i];
				const P2 l{p[i][0] + nrm[i][0] * o, p[i][1] + nrm[i][1] * o}, r{p[i][0] - nrm[i][0] * o, p[i][1] - nrm[i][1] * o};
				left[i] = {l[0], l[1], ground(l[0], l[1]) + 0.05f};
				right[i] = {r[0], r[1], ground(r[0], r[1]) + 0.05f};
				// (on a road - its asphalt or its own sidewalk - the road's surface is there)
				if (onRoad(p[i][0], p[i][1]) || onRoad(l[0], l[1]) || onRoad(r[0], r[1]))
					keep[i] = false;
			}
			// each stretch of kept pieces: the strip (texture across from its left edge, along by distance) and its edges
			// down into the ground under them
			bool any = false;
			for (size_t i = 0; i + 1 < p.size();)
			{
				if (!keep[i] || !keep[i + 1])
				{
					++i;
					continue;
				}
				size_t j = i + 1;
				while (j + 1 < p.size() && keep[j + 1])
					++j;
				const std::vector<P3> rs(right.begin() + i, right.begin() + j + 1), ls(left.begin() + i, left.begin() + j + 1);
				std::vector<std::array<float, 2>> uvR, uvL;
				for (size_t k = i; k <= j; ++k)
					uvR.push_back({width / tm, s[k] / tm}), uvL.push_back({0, s[k] / tm});
				b.Strip(mesh, rs, ls, uvR, uvL, material, false, 2);
				auto lo = ls, ro = rs;
				for (P3& q : lo)
					q[2] -= 0.12f;
				for (P3& q : ro)
					q[2] -= 0.12f;
				b.Wall(out.kerb, lo, 0.12f, true, 0);
				b.Wall(out.kerb, ro, 0.12f, false, 0);
				any = true;
				i = j;
			}
			made += any;
		}
		return made;
	}

	void Paths(const Network& n, float spacing, std::vector<PathNode>& nodes, std::vector<PathLink>& links)
	{
		const auto speedOf = [](const Edge& e) {
			if (paving::Rough(paving::Of(e.surface)))
				return std::min(e.speed > 0 ? e.speed : 9.0f, 9.0f);
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
		// (no traffic made on tracks: the countryside's dirt roads are driven, not busy)
		const auto densityOf = [](const Edge& e) {
			return e.kind.starts_with("motorway") || e.kind.starts_with("primary") || e.kind.starts_with("trunk") ? 5 : e.kind == "service" ? 1
			       : e.kind == "track"                                                                                                ? 0
			                                                                                                                           : 3;
		};
		// (junctions joined by a road too short to show between them are one, as Geometry draws them - the carriageways of
		// a divided road: signalled if any of their vertices has signals, and no traffic light on the roads inside it; a
		// junction to the AI: three streets or more, or signalled - a street past driveways and alleys is not one)
		const size_t nv = n.vertices.size();
		std::vector<int> cluster(nv);
		for (size_t v = 0; v < nv; ++v)
			cluster[v] = static_cast<int>(v);
		const auto root = [&](int v) {
			while (cluster[v] != v)
				v = cluster[v] = cluster[cluster[v]];
			return v;
		};
		std::vector<float> reach(nv, 0);
		for (size_t v = 0; v < nv; ++v)
			for (const int ei : n.vertices[v].edges)
				reach[v] = std::max(reach[v], n.edges[ei].HalfWidth() + n.edges[ei].Walk());
		const auto meeting = [&](int v) { return n.vertices[v].edges.size() >= 3; };
		std::vector<bool> inside(n.edges.size(), false);
		for (size_t ei = 0; ei < n.edges.size(); ++ei)
		{
			const Edge& e = n.edges[ei];
			if (e.a != e.b && meeting(e.a) && meeting(e.b) && Length(e.points) < (reach[e.a] + reach[e.b]) * 1.25f + 2)
				inside[ei] = true, cluster[root(e.a)] = root(e.b);
		}
		std::vector<bool> clusterSignals(nv, false);
		for (size_t v = 0; v < nv; ++v)
			if (n.vertices[v].signals && meeting(static_cast<int>(v)))
				clusterSignals[root(static_cast<int>(v))] = true;
		const auto signalled = [&](int v) { return meeting(v) && clusterSignals[root(v)]; };
		// a merged junction one node, in the middle of its vertices (as the game's own junctions are: the AI makes one
		// junction, with one cycle of lights, of each junction node), its roads inside it left out
		std::vector<P3> middle(nv, P3{0, 0, 0});
		std::vector<int> members(nv, 0);
		std::vector<float> extent(nv, 0);
		for (size_t v = 0; v < nv; ++v)
			if (!n.vertices[v].edges.empty())
			{
				const int r = root(static_cast<int>(v));
				middle[r][0] += n.vertices[v].p[0], middle[r][1] += n.vertices[v].p[1], middle[r][2] += n.vertices[v].z;
				++members[r];
			}
		for (size_t v = 0; v < nv; ++v)
			if (members[v])
				middle[v] = {middle[v][0] / members[v], middle[v][1] / members[v], middle[v][2] / members[v]};
		for (size_t v = 0; v < nv; ++v)
			if (!n.vertices[v].edges.empty())
			{
				const int r = root(static_cast<int>(v));
				extent[r] = std::max(extent[r], std::hypot(n.vertices[v].p[0] - middle[r][0], n.vertices[v].p[1] - middle[r][1]) + reach[v]);
			}
		std::vector<int> vertexNode(n.vertices.size(), -1);
		for (size_t v = 0; v < n.vertices.size(); ++v)
		{
			if (n.vertices[v].edges.empty())
				continue;
			const int r = root(static_cast<int>(v));
			if (members[r] > 1 && vertexNode[r] >= 0)
			{
				vertexNode[v] = vertexNode[r];
				continue;
			}
			PathNode node;
			node.p = members[r] > 1 ? middle[r] : P3{n.vertices[v].p[0], n.vertices[v].p[1], n.vertices[v].z};
			const auto& list = n.vertices[v].edges;
			node.junction = members[r] > 1 || std::count_if(list.begin(), list.end(), [&](int ei) { return !Minor(n.edges[ei]); }) >= 3 ||
			                signalled(static_cast<int>(v));
			if (node.junction)
				node.junctionSize = std::clamp(2 * ((members[r] > 1 ? extent[r] : reach[v]) + 4), 12.0f, 120.0f);
			const Edge& first = n.edges[n.vertices[v].edges[0]];
			node.speed = speedOf(first);
			node.density = densityOf(first);
			node.street = loader::convert::Joaat(first.name);
			vertexNode[v] = static_cast<int>(nodes.size());
			if (members[r] > 1)
				vertexNode[r] = vertexNode[v];
			nodes.push_back(node);
		}
		for (const Edge& e : n.edges)
		{
			if (inside[static_cast<size_t>(&e - n.edges.data())] || (e.a != e.b && vertexNode[e.a] == vertexNode[e.b]))
				continue; // (inside a merged junction)
			const float total = Length(e.points);
			// where its nodes go (distances along it): every `spacing` m, and where the cars coming into a signalled junction
			// stop, a little before its crossing (a node of its own: the game's traffic lights are on such nodes - research/
			// phase0.md §61.27); the plain ones within 4 m of those left out
			std::vector<std::pair<float, bool>> at;
			const int steps = std::max(1, static_cast<int>(std::ceil(total / spacing)));
			for (int k = 1; k < steps; ++k)
				at.push_back({total * k / steps, false});
			const auto stopBack = [&](int v) {
				float across = 0;
				for (const int ei : n.vertices[v].edges)
					if (&n.edges[ei] != &e)
						across = std::max(across, n.edges[ei].HalfWidth() + n.edges[ei].Walk());
				return std::min(across + 4.0f, total * 0.45f);
			};
			std::vector<float> lights;
			if (e.forward > 0 && signalled(e.b) && total > 8)
				lights.push_back(total - stopBack(e.b));
			if (e.backward > 0 && signalled(e.a) && total > 8)
				lights.push_back(stopBack(e.a));
			std::erase_if(at, [&](const auto& x) { return std::any_of(lights.begin(), lights.end(), [&](float l) { return std::fabs(l - x.first) < 4; }); });
			for (const float l : lights)
				at.push_back({l, true});
			std::sort(at.begin(), at.end());
			int previous = vertexNode[e.a];
			size_t i = 0;
			float segStart = 0;
			for (const auto& [want, light] : at)
			{
				while (i + 2 < e.points.size() && segStart + Dist(e.points[i], e.points[i + 1]) < want)
					segStart += Dist(e.points[i], e.points[i + 1]), ++i;
				const float l = Dist(e.points[i], e.points[i + 1]), t = l > 0 ? std::clamp((want - segStart) / l, 0.0f, 1.0f) : 0.0f;
				PathNode node;
				node.p = {e.points[i][0] + (e.points[i + 1][0] - e.points[i][0]) * t, e.points[i][1] + (e.points[i + 1][1] - e.points[i][1]) * t,
				    e.z[i] + (e.z[i + 1] - e.z[i]) * t};
				node.speed = speedOf(e);
				node.density = densityOf(e);
				node.street = loader::convert::Joaat(e.name);
				node.light = light;
				nodes.push_back(node);
				const int here = static_cast<int>(nodes.size() - 1);
				links.push_back({previous, here, e.forward, e.backward, e.LanesHalf() * 2, e.centre * e.laneWidth});
				previous = here;
			}
			links.push_back({previous, vertexNode[e.b], e.forward, e.backward, e.LanesHalf() * 2, e.centre * e.laneWidth});
		}
	}
}
