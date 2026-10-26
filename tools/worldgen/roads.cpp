#include "roads.hpp"

#include <algorithm>
#include <cmath>
#include <map>
#include <unordered_map>

#include "convert/resource.hpp"
#include "polygon.hpp"

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
			       x.kind == y.kind && paving::Of(x.surface) == paving::Of(y.surface);
		}

		// a road that is not a street: a driveway, alley or parking lane (service), a track, a path
		bool Minor(const Edge& e)
		{
			return e.kind == "service" || e.kind == "track" || e.kind == "footway" || e.kind == "path" || e.kind == "cycleway" || e.kind == "pedestrian" ||
			       e.kind == "steps";
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
			void Fan(Mesh& m, const P3& centre, const std::vector<P3>& ring, uint8_t material, bool ground, uint8_t kind = 0)
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
			if (rough)
				fill(sStart, sEnd, 1.0f);
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
				reach[v] = std::max(reach[v], n.edges[ei].HalfWidth() + (n.edges[ei].sidewalks ? kSidewalk : 0));
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
							walk = std::max(walk, n.edges[s].sidewalks ? kSidewalk : 0.0f);
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
			int lanesF = 0, lanesB = 0;
			float laneWidth = 3.5f;
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
			x.sidewalks = o.sidewalks;
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
			float half, walk; // the road's half width, the sidewalk's width (0: none)
			float z;
			bool crossing;
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
			const auto centreOf = [](float h, int lanesF, float lw) { return -h + 0.3f + lanesF * lw; };
			// tapers: where this road changes into a narrower one, its last metres narrow to it and its centre line
			// moves over to the next road's (of two alike wide, the first in the list moves); a sidewalk the next road
			// has not narrows to nothing
			const auto owns = [&](const Next& x) { return x.half < half - 1e-3f || (std::fabs(x.half - half) <= 1e-3f && static_cast<int>(ei) < x.other); };
			const auto taperOf = [&](const Next& x) {
				if (!x.any)
					return 0.0f;
				const float shift = owns(x) ? std::max(half - x.half, std::fabs(centreOf(half, e.forward, w) - centreOf(x.half, x.lanesF, x.laneWidth))) : 0.0f;
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
			std::vector<float> halfAt(p.size()), walkAt(p.size()), centreAt(p.size());
			for (size_t i = 0; i < p.size(); ++i)
			{
				const float ta = into(s[i], true), tb = into(s[i], false);
				float h = half, c = centreOf(half, e.forward, w), wk = e.sidewalks ? kSidewalk : 0;
				for (const auto& [t, x] : {std::pair{ta, &na}, std::pair{tb, &nb}})
				{
					if (t <= 0)
						continue;
					// (the narrower of the two keeps its width: only the wider narrows)
					if (x->half < half)
						h = h + (x->half - half) * t;
					if (owns(*x))
						c = c + (centreOf(x->half, x->lanesF, x->laneWidth) - centreOf(half, e.forward, w)) * t;
					if (e.sidewalks && !x->sidewalks)
						wk = wk * (1 - t);
				}
				halfAt[i] = h, walkAt[i] = wk, centreAt[i] = c;
			}
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
			std::vector<std::array<float, 2>> uvWi, uvWo;
			for (size_t i = 0; i < p.size(); ++i)
			{
				const float k = scale[i], h = halfAt[i], wk = walkAt[i];
				const auto off = [&](float o, float dz) { return P3{p[i][0] + nrm[i][0] * o * k, p[i][1] + nrm[i][1] * o * k, z[i] + dz}; };
				right.push_back(off(-h, 0));
				left.push_back(off(h, 0));
				walkRi.push_back(off(-h, kKerb));
				walkRo.push_back(off(-h - wk, kKerb));
				walkLi.push_back(off(h, kKerb));
				walkLo.push_back(off(h + wk, kKerb));
				uvWi.push_back({0, s[i] / kTexture});
				uvWo.push_back({wk / kTexture, s[i] / kTexture});
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
				const float c0 = centreOf(half, e.forward, w);
				if (e.forward && e.backward && through)
					lines.insert(lines.end(), {c0 - 0.12f, c0 + 0.12f});
				for (int j = 1; j < e.forward; ++j)
					lines.push_back(c0 - j * w);
				for (int j = 1; j < e.backward; ++j)
					lines.push_back(c0 + j * w);
				if (major)
					lines.insert(lines.end(), {-half + 0.45f, half - 0.45f});
				const uint32_t seed = static_cast<uint32_t>(e.id) * 2654435761u ^ static_cast<uint32_t>(std::lround(p.front()[0] * 10)) * 40503u ^
				                      static_cast<uint32_t>(std::lround(p.front()[1] * 10));
				const float f0 = sStart + std::max({clearA + 1.0f, tapA, 1.5f}), f1 = sEnd - std::max({clearB + 1.0f, tapB, 1.5f});
				const float wear = paving::Wear(e.kind, e.smoothness, e.id);
				const auto features = f1 - f0 > 3 ? paving::Features(pv, wear, seed, f0, f1, half, lines, major) : std::vector<paving::Feature>{};
				Carriageway(b, out, pv, features, seed, p, z, s, nrm, scale, halfAt, wear >= 0.5f, !e.bridge);
			}
			if (e.sidewalks)
			{
				b.Strip(out.sidewalk, walkLi, walkLo, uvWi, uvWo, kPavement, !e.bridge, 2); // (left: inner on the right of its direction)
				b.Strip(out.sidewalk, walkRo, walkRi, uvWo, uvWi, kPavement, !e.bridge, 2);
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
			if (e.forward && e.backward && through)
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
				line(offsets([&](size_t i) { return centreAt[i] + j * w; }), 0.2f, kDashU0, kDashU1, 4.0f, from, to, 3.0f, 12.0f, 0.15f, 0.85f);
			}
			if (major)
			{
				// (the right edge of the lanes going along: white; the left edge of a one-way road: yellow)
				const bool oneWay = !e.forward || !e.backward;
				line(offsets([&](size_t i) { return -halfAt[i] + 0.45f; }), 0.2f, oneWay && !e.forward ? kYellowU0 : kWhiteU0, oneWay && !e.forward ? kYellowU1 : kWhiteU1,
				    4.0f, sStart + clearA, sEnd - clearB);
				line(offsets([&](size_t i) { return halfAt[i] - 0.45f; }), 0.2f, oneWay && !e.backward ? kYellowU0 : kWhiteU0, oneWay && !e.backward ? kYellowU1 : kWhiteU1,
				    4.0f, sStart + clearA, sEnd - clearB);
			}
			// at a junction: the crossing (bars along the road, 3 m long, across its width) and the stop line of the
			// lanes coming in behind it
			const auto crossing = [&](bool atB) {
				const size_t i = atB ? p.size() - 1 : 0;
				const float h = halfAt[i];
				for (float o = -h + 0.7f; o <= h - 0.6f; o += 1.2f)
				{
					const std::vector<float> oo(p.size(), o);
					line(oo, 0.55f, kStripeU0, kStripeU1, 2.0f, atB ? sEnd - 3.2f : sStart + 0.2f, atB ? sEnd - 0.2f : sStart + 3.2f);
				}
				const int lanes = atB ? e.forward : e.backward;
				if (!lanes)
					return;
				const float sl = atB ? sEnd - 4.2f : sStart + 4.2f;
				// (the stop line across the lanes coming in: the right of the centre going to b, the left coming to a)
				const float c = centreAt[i];
				const float o0 = atB ? c - lanes * w : c, o1 = atB ? c : c + lanes * w;
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
			// (crossings where people walk: streets with sidewalks, at junctions of three streets or more - not where a
			// street meets only driveways, alleys or parking lanes)
			const bool walked = e.sidewalks && !Minor(e);
			if (walked && crossB && streets[e.b] >= 3)
				crossing(true);
			if (walked && crossA && streets[e.a] >= 3)
				crossing(false);
			// this edge's ends at junctions
			for (const bool atA : {true, false})
			{
				const int v = atA ? e.a : e.b;
				if (kind[v] != Kind::Junction)
					continue;
				const size_t i = atA ? 0 : p.size() - 1, j = atA ? 1 : p.size() - 2;
				ends[root(v)].push_back({static_cast<int>(ei), atA, p[i], Dir(p[i], p[j]), halfAt[i], walkAt[i], z[i], true});
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
				const P2 xr{x.at[0] - lx[0] * x.half, x.at[1] - lx[1] * x.half}, xl{x.at[0] + lx[0] * x.half, x.at[1] + lx[1] * x.half};
				const P2 yr{y.at[0] - ly[0] * y.half, y.at[1] - ly[1] * y.half};
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
				if (x.walk <= 0 && y.walk <= 0)
					continue;
				const P2 xo{xl[0] + lx[0] * x.walk, xl[1] + lx[1] * x.walk}, yo{yr[0] - ly[0] * y.walk, yr[1] - ly[1] * y.walk};
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
						idx.push_back(b.Add(jm, {ring[q][0], ring[q][1], ringZ[q]}, up, {ring[q][0] / tm, -ring[q][1] / tm}));
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
		// asphalt (moved out over the sidewalk, or none), one to a corner; the roads inside a merged junction (between the
		// carriageways of a divided road) have none.
		{
			std::vector<bool> signalled(nv, false);
			for (size_t v = 0; v < nv; ++v)
				if (n.vertices[v].signals && kind[v] == Kind::Junction)
					signalled[root(static_cast<int>(v))] = true;
			// the asphalt's triangles by 8 m cell
			const Collision& col = out.collision;
			const auto cellKey = [](int i, int j) { return static_cast<uint64_t>(static_cast<uint32_t>(i)) << 32 | static_cast<uint32_t>(j); };
			std::unordered_map<uint64_t, std::vector<uint32_t>> cells;
			for (uint32_t t = 0; t < col.triangles.size(); ++t)
			{
				if (col.kinds[t] != 1)
					continue;
				float x0 = 1e30f, y0 = 1e30f, x1 = -1e30f, y1 = -1e30f;
				for (const uint32_t k : col.triangles[t])
					x0 = std::min(x0, col.vertices[k][0]), y0 = std::min(y0, col.vertices[k][1]), x1 = std::max(x1, col.vertices[k][0]),
					y1 = std::max(y1, col.vertices[k][1]);
				for (int j = static_cast<int>(std::floor(y0 / 8)); j <= static_cast<int>(std::floor(y1 / 8)); ++j)
					for (int i = static_cast<int>(std::floor(x0 / 8)); i <= static_cast<int>(std::floor(x1 / 8)); ++i)
						cells[cellKey(i, j)].push_back(t);
			}
			const auto onAsphalt = [&](const P2& q) {
				const auto it = cells.find(cellKey(static_cast<int>(std::floor(q[0] / 8)), static_cast<int>(std::floor(q[1] / 8))));
				if (it == cells.end())
					return false;
				for (const uint32_t t : it->second)
				{
					const P3 &a = col.vertices[col.triangles[t][0]], &b = col.vertices[col.triangles[t][1]], &c = col.vertices[col.triangles[t][2]];
					const auto side = [&](const P3& p0, const P3& p1) { return (p1[0] - p0[0]) * (q[1] - p0[1]) - (p1[1] - p0[1]) * (q[0] - p0[0]); };
					const float s0 = side(a, b), s1 = side(b, c), s2 = side(c, a);
					if ((s0 >= 0 && s1 >= 0 && s2 >= 0) || (s0 <= 0 && s1 <= 0 && s2 <= 0))
						return true;
				}
				return false;
			};
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
					const float along = y ? 1.0f : 4.6f, off = at.half + (at.walk > 0 ? std::min(0.9f, at.walk * 0.35f) : 1.0f);
					P2 q{at.at[0] + at.outward[0] * along + right[0] * off, at.at[1] + at.outward[1] * along + right[1] * off};
					for (int k = 0; k < 8 && onAsphalt(q); ++k)
						q = {q[0] + right[0] * 0.5f, q[1] + right[1] * 0.5f};
					if (onAsphalt(q) || std::any_of(placed.begin(), placed.end(), [&](const P2& p) { return Dist(p, q) < 3; }))
						continue;
					placed.push_back(q);
					out.signals.push_back({{q[0], q[1], at.z + kKerb}, std::atan2(-d[0], d[1]), lanesIn * e.laneWidth + off - at.half});
				}
			}
		}
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
