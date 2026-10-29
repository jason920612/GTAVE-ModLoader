#include "flora.hpp"

#include <algorithm>
#include <cmath>

namespace worldgen::flora
{
	namespace
	{
		using P3 = std::array<float, 3>;
		constexpr float kPi = 3.14159265f;

		// a seeded stream of numbers
		struct Random
		{
			uint32_t s;
			explicit Random(uint32_t seed) : s(seed * 2654435761u + 0x9E3779B9u) { Next(); }
			uint32_t Next()
			{
				s ^= s << 13, s ^= s >> 17, s ^= s << 5;
				return s;
			}
			float Unit() { return (Next() >> 8) / 16777216.0f; }                // [0, 1)
			float Range(float a, float b) { return a + (b - a) * Unit(); }
		};

		P3 Add(const P3& a, const P3& b) { return {a[0] + b[0], a[1] + b[1], a[2] + b[2]}; }
		P3 Sub(const P3& a, const P3& b) { return {a[0] - b[0], a[1] - b[1], a[2] - b[2]}; }
		P3 Mul(const P3& a, float k) { return {a[0] * k, a[1] * k, a[2] * k}; }
		float Dot(const P3& a, const P3& b) { return a[0] * b[0] + a[1] * b[1] + a[2] * b[2]; }
		P3 Cross(const P3& a, const P3& b) { return {a[1] * b[2] - a[2] * b[1], a[2] * b[0] - a[0] * b[2], a[0] * b[1] - a[1] * b[0]}; }
		P3 Unit3(const P3& a)
		{
			const float l = std::sqrt(Dot(a, a));
			return l > 1e-6f ? Mul(a, 1 / l) : P3{0, 0, 1};
		}

		// a limb: points along it and its radius at each
		struct Limb
		{
			std::vector<P3> p;
			std::vector<float> r;
		};

		// A tube of bark along a limb: `sides` round, its texture once round every 1.6 m of girth and along every 3.2 m.
		void Tube(roads::Mesh& m, const Limb& l, int sides)
		{
			if (l.p.size() < 2)
				return;
			const uint32_t base = static_cast<uint32_t>(m.vertices.size());
			float along = 0;
			P3 prevSide{1, 0, 0};
			for (size_t i = 0; i < l.p.size(); ++i)
			{
				const P3 t = Unit3(Sub(l.p[std::min(i + 1, l.p.size() - 1)], l.p[i ? i - 1 : 0]));
				// (a side vector carried along the limb, so the rings do not twist)
				P3 side = Sub(prevSide, Mul(t, Dot(prevSide, t)));
				if (Dot(side, side) < 1e-4f)
					side = Cross(t, std::fabs(t[2]) < 0.9f ? P3{0, 0, 1} : P3{1, 0, 0});
				side = Unit3(side);
				prevSide = side;
				const P3 up = Cross(t, side);
				if (i)
					along += std::sqrt(Dot(Sub(l.p[i], l.p[i - 1]), Sub(l.p[i], l.p[i - 1])));
				const float around = std::max(1.0f, std::round(2 * kPi * l.r[0] / 1.6f));
				for (int k = 0; k <= sides; ++k)
				{
					const float a = 2 * kPi * k / sides;
					const P3 n = Add(Mul(side, std::cos(a)), Mul(up, std::sin(a)));
					ydr::Vertex v;
					v.position = Add(l.p[i], Mul(n, l.r[i]));
					v.normal = n;
					v.tangent = {t[0], t[1], t[2], 1};
					v.colour0 = {255, 255, 255, 255};
					v.uv[0] = {around * k / sides, -along / 3.2f};
					m.vertices.push_back(v);
				}
			}
			for (size_t i = 0; i + 1 < l.p.size(); ++i)
				for (int k = 0; k < sides; ++k)
				{
					const uint32_t a = base + static_cast<uint32_t>(i * (sides + 1) + k), b = a + 1, c = a + sides + 1, d = c + 1;
					m.indices.insert(m.indices.end(), {a, c, d, a, d, b});
				}
		}

		// A card of a cluster picture (one of the atlas's four), `size` m square, centred at `c`, its face towards `facing`
		// (turned `spin` about it), both faces; every vertex's normal `light` (out from the crown's middle: lit as the crown).
		// `hanging`: its top edge at c (moss).
		void Card(roads::Mesh& m, const P3& c, const P3& facing, float spin, float width, float height, const P3& light, int cell, bool hanging = false)
		{
			const P3 f = Unit3(facing);
			P3 u = Cross(f, std::fabs(f[2]) < 0.95f ? P3{0, 0, 1} : P3{1, 0, 0});
			u = Unit3(u);
			P3 v = Cross(u, f);
			if (v[2] < 0)
				v = Mul(v, -1); // (the picture's up, up)
			const float cs = std::cos(spin), sn = std::sin(spin);
			const P3 ru = Add(Mul(u, cs), Mul(v, sn)), rv = Add(Mul(v, cs), Mul(u, -sn));
			const P3 hu = Mul(hanging ? u : ru, width / 2), hv = Mul(hanging ? v : rv, height / 2);
			const P3 mid = hanging ? Sub(c, hv) : c;
			const float u0 = (cell % 2) * 0.5f, v0 = (cell / 2) * 0.5f;
			const P3 corners[4] = {Sub(Sub(mid, hu), hv), Sub(Add(mid, hu), hv), Add(Add(mid, hu), hv), Add(Sub(mid, hu), hv)};
			const std::array<float, 2> uvs[4] = {{u0 + 0.01f, v0 + 0.49f}, {u0 + 0.49f, v0 + 0.49f}, {u0 + 0.49f, v0 + 0.01f}, {u0 + 0.01f, v0 + 0.01f}};
			const P3 n = Unit3(light);
			// (the game's tree shader reads its vertices' colours - the oak's leaves: red 0-105, green 64-126, blue near 0, alpha
			// 220-235; the second colour's blue 0-255, the rest 0 -: as those, a card's own within them)
			const uint32_t h = static_cast<uint32_t>(std::fabs(c[0] * 131 + c[1] * 71 + c[2] * 37));
			const std::array<uint8_t, 4> c0{static_cast<uint8_t>(40 + h % 36), static_cast<uint8_t>(80 + (h >> 7) % 28), static_cast<uint8_t>((h >> 13) % 12), 228};
			const std::array<uint8_t, 4> c1{0, 0, static_cast<uint8_t>((h >> 17) % 120), 0};
			for (int face = 0; face < 2; ++face)
			{
				const uint32_t base = static_cast<uint32_t>(m.vertices.size());
				for (int k = 0; k < 4; ++k)
				{
					ydr::Vertex x;
					x.position = corners[k];
					x.normal = n;
					const P3 t = face ? Mul(hu, -1) : hu;
					const P3 tu = Unit3(t);
					x.tangent = {tu[0], tu[1], tu[2], 1};
					x.colour0 = c0;
					x.colour1 = c1;
					x.uv[0] = uvs[k];
					m.vertices.push_back(x);
				}
				if (face == 0)
					m.indices.insert(m.indices.end(), {base, base + 1, base + 2, base, base + 2, base + 3});
				else
					m.indices.insert(m.indices.end(), {base, base + 2, base + 1, base, base + 3, base + 2});
			}
		}

		// A palm's frond: a strip from `base` along an arc - leaving it `rise` (radians) over level towards `azimuth`, bending
		// down `droop` more by its tip -, `length` m long, `width` across at its widest, its leaflets an inverted V (the edges
		// `fold` of the half width under the rachis); the atlas cell's picture along it (its base at the cell's foot), both
		// faces, lit out from the crown's middle, in `segments` along it.
		void Frond(roads::Mesh& m, const P3& base, float azimuth, float rise, float droop, float length, float width, float fold, const P3& middle, int cell,
		    int segments)
		{
			const P3 side{-std::sin(azimuth), std::cos(azimuth), 0};
			const float u0 = (cell % 2) * 0.5f, v0 = (cell / 2) * 0.5f;
			std::vector<P3> centre{base};
			for (int i = 0; i < segments; ++i)
			{
				const float t = (i + 0.5f) / segments, a = rise - droop * t * t;
				const P3 dir{std::cos(azimuth) * std::cos(a), std::sin(azimuth) * std::cos(a), std::sin(a)};
				centre.push_back(Add(centre.back(), Mul(dir, length / segments)));
			}
			// (the game's tree shader's colours, as a card's)
			const uint32_t h = static_cast<uint32_t>(std::fabs(base[0] * 131 + base[1] * 71 + azimuth * 997));
			const std::array<uint8_t, 4> c0{static_cast<uint8_t>(40 + h % 36), static_cast<uint8_t>(80 + (h >> 7) % 28), static_cast<uint8_t>((h >> 13) % 12), 228};
			const std::array<uint8_t, 4> c1{0, 0, static_cast<uint8_t>(60 + (h >> 17) % 120), 0};
			const float us[3] = {u0 + 0.01f, u0 + 0.25f, u0 + 0.49f};
			for (int face = 0; face < 2; ++face)
			{
				const uint32_t first = static_cast<uint32_t>(m.vertices.size());
				for (int i = 0; i <= segments; ++i)
				{
					const float t = static_cast<float>(i) / segments;
					// (narrow at its base and tip, widest past its middle)
					const float half = width / 2 * std::max(0.06f, std::sin(kPi * std::min(1.0f, 0.12f + 0.92f * t)));
					const P3& c = centre[static_cast<size_t>(i)];
					P3 light = Sub(c, middle);
					light[2] = light[2] * 0.6f + 0.5f * std::sqrt(Dot(light, light));
					const P3 n = Unit3(light);
					const P3 edges[3] = {Add(Sub(c, Mul(side, half)), P3{0, 0, -fold * half}), c, Add(Add(c, Mul(side, half)), P3{0, 0, -fold * half})};
					const P3 tu = face ? Mul(side, -1) : side;
					for (int k = 0; k < 3; ++k)
					{
						ydr::Vertex x;
						x.position = edges[k];
						x.normal = n;
						x.tangent = {tu[0], tu[1], tu[2], 1};
						x.colour0 = c0;
						x.colour1 = c1;
						x.uv[0] = {us[k], v0 + 0.49f - 0.48f * t};
						m.vertices.push_back(x);
					}
				}
				for (int i = 0; i < segments; ++i)
				{
					const uint32_t a = first + static_cast<uint32_t>(i) * 3, b = a + 3; // (this station's left, middle, right; the next one's)
					if (face == 0)
						m.indices.insert(m.indices.end(), {a, b, b + 1, a, b + 1, a + 1, a + 1, b + 1, b + 2, a + 1, b + 2, a + 2});
					else
						m.indices.insert(m.indices.end(), {a, b + 1, b, a, a + 1, b + 1, a + 1, b + 2, b + 1, a + 1, a + 2, b + 2});
				}
			}
		}

		// a limb from `from` heading `dir` (curving by `bend` each step towards `pull`), `length` m in `steps`, its radius
		// from r0 to r1
		Limb Grow(const P3& from, P3 dir, const P3& pull, float bend, float length, int steps, float r0, float r1, Random& rnd, float wiggle = 0.15f)
		{
			Limb l;
			P3 p = from;
			dir = Unit3(dir);
			for (int i = 0; i <= steps; ++i)
			{
				const float t = static_cast<float>(i) / steps;
				l.p.push_back(p);
				l.r.push_back(r0 + (r1 - r0) * t);
				dir = Unit3(Add(Add(dir, Mul(Unit3(pull), bend)), P3{rnd.Range(-wiggle, wiggle), rnd.Range(-wiggle, wiggle), rnd.Range(-wiggle, wiggle) * 0.5f}));
				p = Add(p, Mul(dir, length / steps));
			}
			return l;
		}

		// the point at share t along a limb
		P3 At(const Limb& l, float t)
		{
			const float f = std::clamp(t, 0.0f, 1.0f) * (l.p.size() - 1);
			const size_t i = std::min(static_cast<size_t>(f), l.p.size() - 2);
			const float k = f - i;
			return Add(l.p[i], Mul(Sub(l.p[i + 1], l.p[i]), k));
		}
		float RadiusAt(const Limb& l, float t)
		{
			const float f = std::clamp(t, 0.0f, 1.0f) * (l.r.size() - 1);
			const size_t i = std::min(static_cast<size_t>(f), l.r.size() - 2);
			return l.r[i] + (l.r[i + 1] - l.r[i]) * (f - i);
		}

		// the levels of detail of a set of limbs and cards: tubes of 8, 5, 4 sides (the thinnest left out further off), every
		// card near, one in two a third bigger, one in five 1.9 times, none at the last level but the biggest (one in twelve,
		// 2.6 times: the silhouette)
		struct Leaf
		{
			P3 c, facing, light;
			float spin, w, h;
			int cell;
			Texture tex;
			bool hanging;
		};
		void Finish(Model& out, const std::vector<std::pair<Limb, Texture>>& limbs, const std::vector<Leaf>& leaves)
		{
			const int sides[4] = {8, 5, 4, 3};
			const float minRadius[4] = {0, 0.04f, 0.1f, 0.2f};
			for (int lod = 0; lod < 4; ++lod)
			{
				for (const auto& [l, tex] : limbs)
					if (l.r[0] >= minRadius[lod])
						Tube(out.lods[lod][tex], l, l.r[0] > 0.25f ? sides[lod] + (lod == 0 ? 2 : 0) : std::max(3, sides[lod] - 2));
				const int every[4] = {1, 2, 4, 8};
				const float grow[4] = {1.0f, 1.32f, 1.7f, 2.1f};
				for (size_t k = 0; k < leaves.size(); k += every[lod])
				{
					const Leaf& f = leaves[k];
					if (lod >= 2 && f.hanging && (k / every[lod]) % 2)
						continue;
					Card(out.lods[lod][f.tex], f.c, f.facing, f.spin, f.w * grow[lod], f.h * (f.hanging ? std::sqrt(grow[lod]) : grow[lod]), f.light, f.cell,
					    f.hanging);
				}
			}
		}

		// a palm's levels of detail: its trunk and stalks (and a cabbage palm's fans) as Finish's limbs and cards, its fronds
		// in fewer segments further off (8, 5, 3, 2) - each frond: azimuth, rise, droop, length, width, cell
		void FinishPalm(Model& out, const std::vector<std::pair<Limb, Texture>>& limbs, const std::vector<std::array<float, 6>>& fronds, const P3& crown,
		    const P3& middle, float fold, const std::vector<Leaf>& fans)
		{
			Finish(out, limbs, fans);
			const int segments[4] = {8, 5, 3, 2};
			for (int lod = 0; lod < 4; ++lod)
				for (const auto& f : fronds)
					Frond(out.lods[static_cast<size_t>(lod)][kPalmFrond], crown, f[0], f[1], f[2], f[3], f[4], fold, middle, static_cast<int>(f[5]), segments[lod]);
		}

		// cards round a point on a limb: their faces any way, lit out from the crown's middle
		void Cluster(std::vector<Leaf>& leaves, const P3& at, const P3& middle, int count, float spread, float size, Texture tex, Random& rnd, float upward = 0.3f)
		{
			for (int k = 0; k < count; ++k)
			{
				const P3 off{rnd.Range(-spread, spread), rnd.Range(-spread, spread), rnd.Range(-spread * 0.5f, spread) + upward};
				const P3 c = Add(at, off);
				const P3 facing{rnd.Range(-1, 1), rnd.Range(-1, 1), rnd.Range(-0.6f, 1)};
				P3 light = Sub(c, middle);
				light[2] = light[2] * 0.7f + 0.6f * std::sqrt(Dot(light, light)) * 0.3f;
				const float s = size * rnd.Range(0.8f, 1.2f);
				leaves.push_back({c, facing, light, rnd.Range(0, 2 * kPi), s, s, static_cast<int>(rnd.Next() % 4), tex, false});
			}
		}

		// A live oak: a trunk to its first fork 2.4-3.6 m up (a town's are pruned clear of the street: the street pictures'
		// leaves start 3-4 m up), four to six huge limbs out almost level and drooping at their ends, the crown wide and low
		// (as wide as 1.3-1.8 times its height), Spanish moss hanging from the limbs.
		Model LiveOak(uint32_t seed)
		{
			Random rnd(seed);
			Model out;
			const float H = rnd.Range(12, 16), W = H * rnd.Range(1.3f, 1.8f), fork = rnd.Range(2.4f, 3.6f);
			// (Spanish moss on some: a quarter of the live oaks none, a quarter a little, the rest hung with it - every tree's
			// had been "stiff pale strips on every tree, residential ones that have none too")
			const uint32_t mossHash = seed * 2654435761u;
			const float moss = (mossHash >> 13) % 4 == 0 ? 0.0f : (mossHash >> 13) % 4 == 1 ? 0.2f : 0.45f;
			std::vector<std::pair<Limb, Texture>> limbs;
			std::vector<Leaf> leaves;
			const P3 lean{rnd.Range(-0.12f, 0.12f), rnd.Range(-0.12f, 0.12f), 1};
			Limb trunk = Grow({0, 0, -0.3f}, lean, {0, 0, 1}, 0.05f, fork + 0.3f, 3, rnd.Range(0.55f, 0.75f), 0.48f, rnd, 0.05f);
			limbs.push_back({trunk, kBarkOak});
			const P3 top = trunk.p.back(), middle{top[0], top[1], H * 0.6f};
			// (the trunk going on up into the fork, thinning: its top no flat cut between the limbs)
			limbs.push_back({Grow(top, lean, {0, 0, 1}, 0.05f, 1.4f, 2, 0.46f, 0.16f, rnd, 0.05f), kBarkOak});
			const int n = 4 + static_cast<int>(rnd.Next() % 3);
			const float az0 = rnd.Range(0, 2 * kPi);
			for (int i = 0; i < n; ++i)
			{
				const float az = az0 + 2 * kPi * i / n + rnd.Range(-0.35f, 0.35f);
				const P3 out2{std::cos(az), std::sin(az), 0};
				const float len = W * 0.5f * rnd.Range(0.85f, 1.1f);
				// (out and up steeply, then bending over to level and down at the end)
				// (up at 40-55 degrees, levelling off over its length: a hammock's swoop where it bent down past level)
				Limb limb = Grow(Add(top, Mul(out2, 0.25f)), Add(Mul(out2, 0.75f), P3{0, 0, rnd.Range(0.65f, 0.9f)}), Add(out2, P3{0, 0, -0.06f}), 0.11f, len, 8,
				    0.36f, 0.07f, rnd, 0.06f);
				// (its height kept under the crown's top and, 2 m out from the trunk on, 1.8 m over the fork: the street
				// pictures' live oaks' limbs sweep out level, they do not come down to the ground - and they grow out of the
				// trunk's top: a floor under them nearer had lifted them off it, the crown floating over a cut stump)
				for (P3& q : limb.p)
				{
					q[2] = std::min(q[2], H - 1.5f);
					if (std::hypot(q[0] - top[0], q[1] - top[1]) > 2.0f)
						q[2] = std::max(q[2], fork + 1.8f);
				}
				limbs.push_back({limb, kBarkOak});
				// (its leaves in many small cards - 0.6-0.9 m, twice as many as before: a few big ones had read as "one generic
				// big-leaf tree", each card's outline a giant leaf)
				for (float t = 0.3f; t < 1.0f; t += 0.07f)
				{
					const P3 at = At(limb, t);
					Cluster(leaves, at, middle, 16, 1.5f, rnd.Range(0.62f, 0.88f), kOakLeaves, rnd, 0.5f);
					// (moss under the limb)
					if (t > 0.35f && t < 0.92f && rnd.Unit() < moss)
					{
						const P3 drop{at[0] + rnd.Range(-0.4f, 0.4f), at[1] + rnd.Range(-0.4f, 0.4f), at[2] - RadiusAt(limb, t)};
						const float len2 = rnd.Range(0.6f, 2.2f), wd = rnd.Range(0.2f, 0.36f);
						const float spin = rnd.Range(0, kPi);
						const P3 light{at[0] - top[0], at[1] - top[1], 0.3f};
						for (int c = 0; c < 2; ++c)
							leaves.push_back({drop, {std::cos(spin + c * kPi / 2), std::sin(spin + c * kPi / 2), 0}, light, 0, wd, len2,
							    static_cast<int>(rnd.Next() % 4), kMoss, true});
					}
				}
				// side branches: two or three, out to the sides and up a little
				const int sideCount = 2 + static_cast<int>(rnd.Next() % 2);
				for (int k = 0; k < sideCount; ++k)
				{
					const float t = rnd.Range(0.3f, 0.75f);
					const float turn = (k % 2 ? 1.0f : -1.0f) * rnd.Range(0.5f, 0.9f);
					const P3 sideDir{std::cos(az + turn), std::sin(az + turn), rnd.Range(0.1f, 0.35f)};
					Limb br = Grow(At(limb, t), sideDir, {sideDir[0], sideDir[1], -0.05f}, 0.1f, len * rnd.Range(0.35f, 0.5f), 4, RadiusAt(limb, t) * 0.55f, 0.04f,
					    rnd, 0.12f);
					for (size_t q = 1; q < br.p.size(); ++q) // (not where it leaves the limb)
						br.p[q][2] = std::clamp(br.p[q][2], fork + 1.6f, H - 1.2f);
					limbs.push_back({br, kBarkOak});
					for (float s = 0.25f; s < 1.0f; s += 0.12f)
					{
						const P3 at = At(br, s);
						Cluster(leaves, at, middle, 12, 1.2f, rnd.Range(0.6f, 0.85f), kOakLeaves, rnd, 0.4f);
						if (s < 0.85f && rnd.Unit() < moss * 0.75f)
						{
							const P3 drop{at[0], at[1], at[2] - 0.05f};
							const float spin = rnd.Range(0, kPi);
							const P3 light{at[0] - top[0], at[1] - top[1], 0.3f};
							for (int c = 0; c < 2; ++c)
								leaves.push_back({drop, {std::cos(spin + c * kPi / 2), std::sin(spin + c * kPi / 2), 0}, light, 0, rnd.Range(0.18f, 0.32f),
								    rnd.Range(0.5f, 1.8f), static_cast<int>(rnd.Next() % 4), kMoss, true});
						}
					}
				}
			}
			// (the crown's top filled in over the fork, and a shell of leaves over the whole crown - a flattened dome: the
			// street pictures' live oaks are a closed dark mass, no sky through them)
			for (int k = 0; k < 10; ++k)
			{
				const float a = rnd.Range(0, 2 * kPi), d = rnd.Range(0, W * 0.25f);
				Cluster(leaves, {top[0] + std::cos(a) * d, top[1] + std::sin(a) * d, H * rnd.Range(0.7f, 0.85f)}, middle, 10, 1.5f, 0.88f, kOakLeaves, rnd, 0.3f);
			}
			// (the shell lumpy: its reach and its height waving round the tree - three to five big lobes, a dip or two in its
			// top -, its underside only out past the limbs' middles: the street pictures' oaks are no domes, the reviewers saw
			// "umbrella trees")
			const float p1 = rnd.Range(0, 2 * kPi), p2 = rnd.Range(0, 2 * kPi), p3 = rnd.Range(0, 2 * kPi);
			const int lobes = 3 + static_cast<int>(rnd.Next() % 3);
			for (int k = 0; k < 560; ++k)
			{
				const float a = rnd.Range(0, 2 * kPi), up = std::asin(rnd.Range(-0.15f, 1.0f));
				const float reach = 1 + 0.16f * std::sin(lobes * a + p1) + 0.08f * std::sin((lobes + 2) * a + p2);
				const float rise = (H - fork) * (0.1f * std::sin(2 * a + p3) - 0.06f * std::sin(lobes * a + p2));
				const float rx = W * 0.47f * std::cos(up) * reach, rz = (H - fork) * 0.55f * std::sin(up);
				if (up < 0.1f && rnd.Unit() < 0.5f)
					continue; // (the underside thinner: the limbs seen between)
				const P3 c{top[0] + std::cos(a) * rx, top[1] + std::sin(a) * rx, std::min(H - 0.6f, fork + (H - fork) * 0.45f + rz + rise * std::sin(up))};
				Cluster(leaves, c, middle, 2, 0.3f, rnd.Range(0.66f, 0.92f), kOakLeaves, rnd, 0.0f);
			}
			Finish(out, limbs, leaves);
			out.height = H, out.width = W, out.trunk = trunk.r[0];
			return out;
		}

		// A loblolly pine: a straight trunk bare to its crown (the top third or so: the street pictures' pines have nothing
		// below 15 m), whorls of branches swept up, shorter towards the top, needles in tufts at their ends; a few dead stubs
		// lower down.
		Model Pine(uint32_t seed)
		{
			Random rnd(seed);
			Model out;
			const float H = rnd.Range(22, 30), crownBase = H * rnd.Range(0.6f, 0.7f), W = rnd.Range(6.5f, 9.5f);
			std::vector<std::pair<Limb, Texture>> limbs;
			std::vector<Leaf> leaves;
			Limb trunk = Grow({0, 0, -0.3f}, {rnd.Range(-0.03f, 0.03f), rnd.Range(-0.03f, 0.03f), 1}, {0, 0, 1}, 0.02f, H + 0.3f, 10, rnd.Range(0.28f, 0.36f), 0.04f,
			    rnd, 0.025f);
			limbs.push_back({trunk, kBarkPine});
			const P3 middle{trunk.p.back()[0] * 0.8f, trunk.p.back()[1] * 0.8f, crownBase + (H - crownBase) * 0.45f};
			// dead stubs below the crown
			for (int k = 0; k < 8; ++k)
			{
				const float z = rnd.Range(H * 0.35f, crownBase), a = rnd.Range(0, 2 * kPi);
				const P3 at = At(trunk, (z + 0.3f) / (H + 0.3f));
				limbs.push_back({Grow(at, {std::cos(a), std::sin(a), rnd.Range(-0.2f, 0.2f)}, {0, 0, -1}, 0.1f, rnd.Range(0.3f, 0.9f), 1, 0.03f, 0.012f, rnd), kBarkPine});
			}
			// the crown: a whorl every 0.9-1.3 m
			for (float z = crownBase; z < H - 0.6f; z += rnd.Range(0.9f, 1.3f))
			{
				const float up = (z - crownBase) / (H - crownBase);
				const float reach = W * 0.5f * std::pow(1 - up, 0.55f) * rnd.Range(0.75f, 1.05f) + 0.4f;
				const P3 at = At(trunk, (z + 0.3f) / (H + 0.3f));
				const int count = 3 + static_cast<int>(rnd.Next() % 2);
				const float a0 = rnd.Range(0, 2 * kPi);
				for (int k = 0; k < count; ++k)
				{
					const float a = a0 + 2 * kPi * k / count + rnd.Range(-0.4f, 0.4f);
					const P3 dir{std::cos(a), std::sin(a), rnd.Range(0.2f, 0.65f)};
					Limb br = Grow(at, dir, {dir[0], dir[1], 0.6f}, 0.12f, reach, 3, 0.07f * (1 - up * 0.6f), 0.015f, rnd, 0.12f);
					limbs.push_back({br, kBarkPine});
					for (float t = 0.35f; t <= 1.0f; t += 0.15f)
						Cluster(leaves, At(br, t), middle, 3, 0.55f, rnd.Range(1.5f, 2.1f), kPineNeedles, rnd, 0.15f);
				}
			}
			// (its top tuft)
			Cluster(leaves, trunk.p.back(), middle, 4, 0.5f, 1.6f, kPineNeedles, rnd, 0.0f);
			Finish(out, limbs, leaves);
			out.height = H, out.width = W, out.trunk = trunk.r[0];
			return out;
		}

		// A laurel or water oak: a trunk to 3-4.5 m going on up as a leader through the crown, four or five limbs climbing at
		// 30-50 degrees out of it at different heights and forking, a round crown about as wide as two thirds of its height
		// (three limbs from one point had read as "one generic Y-forked tree" to every reviewer).
		Model Oak(uint32_t seed)
		{
			Random rnd(seed);
			Model out;
			const float H = rnd.Range(14, 20), W = H * rnd.Range(0.65f, 0.85f), fork = rnd.Range(3.0f, 4.5f);
			std::vector<std::pair<Limb, Texture>> limbs;
			std::vector<Leaf> leaves;
			Limb trunk = Grow({0, 0, -0.3f}, {rnd.Range(-0.06f, 0.06f), rnd.Range(-0.06f, 0.06f), 1}, {0, 0, 1}, 0.05f, fork + 0.3f, 3, rnd.Range(0.35f, 0.45f), 0.3f,
			    rnd, 0.05f);
			limbs.push_back({trunk, kBarkOak});
			const P3 top = trunk.p.back(), middle{top[0], top[1], fork + (H - fork) * 0.5f};
			// (the leader: on up through the crown, thinning)
			const Limb leader = Grow(top, {rnd.Range(-0.08f, 0.08f), rnd.Range(-0.08f, 0.08f), 1}, {0, 0, 1}, 0.06f, (H - fork) * 0.7f, 5, 0.3f, 0.06f, rnd, 0.08f);
			limbs.push_back({leader, kBarkOak});
			for (float t = 0.5f; t <= 1.0f; t += 0.1f)
				Cluster(leaves, At(leader, t), middle, 9, 1.2f, rnd.Range(0.7f, 1.0f), kOakLeaves, rnd, 0.3f);
			const int n = 4 + static_cast<int>(rnd.Next() % 2);
			const float az0 = rnd.Range(0, 2 * kPi);
			for (int i = 0; i < n; ++i)
			{
				const float az = az0 + 2 * kPi * i / n + rnd.Range(-0.3f, 0.3f);
				const float tilt = rnd.Range(0.75f, 1.2f);
				const P3 dir{std::cos(az) * tilt, std::sin(az) * tilt, 1};
				const float len = std::hypot(W * 0.42f, H - fork) * rnd.Range(0.7f, 0.9f);
				// (each out of the leader at its own height: no fork of them all at one point)
				const P3 from = At(leader, rnd.Range(0.0f, 0.35f));
				Limb limb = Grow(from, dir, {std::cos(az), std::sin(az), 0.2f}, 0.1f, len, 6, 0.22f, 0.05f, rnd, 0.1f);
				limbs.push_back({limb, kBarkOak});
				for (int k = 0; k < 3; ++k)
				{
					const float t = rnd.Range(0.35f, 0.8f), turn = rnd.Range(-1.0f, 1.0f);
					const P3 d2{std::cos(az + turn), std::sin(az + turn), rnd.Range(0.3f, 0.9f)};
					Limb br = Grow(At(limb, t), d2, {d2[0], d2[1], 0}, 0.15f, len * rnd.Range(0.35f, 0.55f), 4, RadiusAt(limb, t) * 0.55f, 0.03f, rnd);
					limbs.push_back({br, kBarkOak});
					for (float s = 0.3f; s <= 1.0f; s += 0.14f)
						Cluster(leaves, At(br, s), middle, 9, 1.1f, rnd.Range(0.7f, 1.0f), kOakLeaves, rnd, 0.3f);
				}
				for (float t = 0.45f; t <= 1.0f; t += 0.1f)
					Cluster(leaves, At(limb, t), middle, 9, 1.3f, rnd.Range(0.7f, 1.0f), kOakLeaves, rnd, 0.3f);
			}
			// (the round crown closed: a shell of leaves over it and some through it - the reviewers' "lollipops", a bare trunk
			// under a few clumps, were the limbs' leaves alone)
			const float cz = fork + (H - fork) * 0.55f, rh = W * 0.5f, rv = (H - fork) * 0.5f;
			for (int k = 0; k < 420; ++k)
			{
				const float a = rnd.Range(0, 2 * kPi), up = std::asin(rnd.Range(-0.55f, 1.0f)), depth = k % 4 ? 1.0f : rnd.Range(0.4f, 0.85f);
				const P3 c{top[0] + std::cos(a) * rh * std::cos(up) * depth, top[1] + std::sin(a) * rh * std::cos(up) * depth, cz + rv * std::sin(up) * depth};
				Cluster(leaves, c, middle, 2, 0.3f, rnd.Range(0.7f, 0.95f), kOakLeaves, rnd, 0.0f);
			}
			Finish(out, limbs, leaves);
			out.height = H, out.width = W, out.trunk = trunk.r[0];
			return out;
		}

		// A crape myrtle: three to five thin smooth stems out of the ground in a vase, leaves on their upper half - and on one
		// in four the summer's flowers (crimson, pink, white or lavender: one of the atlas's cells) over them: the street
		// pictures are of every season, and every one in bloom had read as "pink crape myrtles everywhere".
		Model Myrtle(uint32_t seed)
		{
			Random rnd(seed);
			Model out;
			const float H = rnd.Range(4.5f, 7.5f), W = H * rnd.Range(0.7f, 0.9f);
			std::vector<std::pair<Limb, Texture>> limbs;
			std::vector<Leaf> leaves;
			const P3 middle{0, 0, H * 0.65f};
			const int n = 3 + static_cast<int>(rnd.Next() % 3);
			const int bloom = static_cast<int>(rnd.Next() % 4);
			const bool flowering = rnd.Next() % 4 == 0;
			for (int i = 0; i < n; ++i)
			{
				const float az = 2 * kPi * i / n + rnd.Range(-0.4f, 0.4f);
				const P3 dir{std::cos(az) * 0.22f, std::sin(az) * 0.22f, 1};
				Limb stem = Grow({std::cos(az) * 0.12f, std::sin(az) * 0.12f, -0.2f}, dir, {std::cos(az), std::sin(az), 0.4f}, 0.07f, H * rnd.Range(0.8f, 0.95f), 5,
				    rnd.Range(0.09f, 0.13f), 0.035f, rnd, 0.08f);
				limbs.push_back({stem, kBarkMyrtle});
				for (float t = 0.5f; t <= 1.0f; t += 0.1f)
				{
					// (many small cards, not a few big ones: the reviewers' "giant leaf cards on sticks")
					Cluster(leaves, At(stem, t), middle, flowering ? 4 : 6, 0.9f, rnd.Range(0.6f, 0.85f), kOakLeaves, rnd, 0.2f);
					// (the flowers: their panicles on top)
					if (!flowering)
						continue;
					Leaf f;
					const P3 c = Add(At(stem, t), P3{rnd.Range(-0.6f, 0.6f), rnd.Range(-0.6f, 0.6f), rnd.Range(0.1f, 0.6f)});
					f = {c, {rnd.Range(-1, 1), rnd.Range(-1, 1), rnd.Range(0, 1)}, Sub(c, middle), rnd.Range(0, 2 * kPi), 1.2f, 1.2f, bloom, kMyrtle, false};
					leaves.push_back(f);
				}
			}
			out.width = W;
			Finish(out, limbs, leaves);
			out.height = H, out.trunk = 0.1f;
			return out;
		}

		// A southern magnolia: a straight trunk most of the way up, branches every 0.6-0.9 m climbing at 40-60 degrees, a
		// dense dark crown like an egg - widest a third of the way up, pointed at its top, as wide as 0.55-0.7 of the height -
		// from 1.6-2.6 m up (a town's limbed up); big glossy leaves, a few white flowers.
		Model Magnolia(uint32_t seed)
		{
			Random rnd(seed);
			Model out;
			const float H = rnd.Range(9, 16), W = H * rnd.Range(0.55f, 0.7f), base = rnd.Range(2.6f, 3.4f);
			std::vector<std::pair<Limb, Texture>> limbs;
			std::vector<Leaf> leaves;
			const float tall = H * 0.78f + 0.3f;
			Limb trunk = Grow({0, 0, -0.3f}, {rnd.Range(-0.04f, 0.04f), rnd.Range(-0.04f, 0.04f), 1}, {0, 0, 1}, 0.03f, tall, 6, rnd.Range(0.24f, 0.34f), 0.07f,
			    rnd, 0.03f);
			limbs.push_back({trunk, kBarkSmooth});
			const P3 middle{0, 0, base + (H - base) * 0.4f};
			// (the crown's reach at a share v of its height: an egg)
			const auto reachAt = [&](float v) { return W * 0.5f * std::min(1.0f, 2.2f * std::pow(std::max(0.0f, 1 - v), 0.8f) * (0.35f + v)); };
			for (float z = base; z < H * 0.82f; z += rnd.Range(0.6f, 0.9f))
			{
				const float v = (z - base) / (H - base);
				const float a = rnd.Range(0, 2 * kPi);
				const P3 at = At(trunk, (z + 0.3f) / tall);
				const P3 dir{std::cos(a), std::sin(a), rnd.Range(0.7f, 1.2f)};
				Limb br = Grow(at, dir, {std::cos(a), std::sin(a), 0.4f}, 0.1f, reachAt(v) * rnd.Range(0.85f, 1.1f), 3, 0.08f * (1 - v * 0.6f), 0.02f, rnd, 0.1f);
				limbs.push_back({br, kBarkSmooth});
				for (float t = 0.35f; t <= 1.0f; t += 0.3f)
					Cluster(leaves, At(br, t), middle, 3, 0.7f, rnd.Range(1.0f, 1.35f), kMagnoliaLeaves, rnd, 0.15f);
			}
			// (the egg's shell, and some through it: a closed dark mass)
			for (int k = 0; k < 460; ++k)
			{
				const float a = rnd.Range(0, 2 * kPi), v = std::pow(rnd.Unit(), 0.85f);
				const float r = reachAt(v) * (k % 4 ? rnd.Range(0.9f, 1.05f) : rnd.Range(0.45f, 0.85f));
				Cluster(leaves, {std::cos(a) * r, std::sin(a) * r, base + 0.3f + (H - base - 0.3f) * v}, middle, 1, 0.25f, rnd.Range(1.0f, 1.3f), kMagnoliaLeaves, rnd, 0.0f);
			}
			Finish(out, limbs, leaves);
			out.height = H, out.width = W, out.trunk = trunk.r[0];
			return out;
		}

		// A sweetgum: a central leader up the whole tree, branches out level and a little up in loose whorls every 0.8-1.3 m,
		// shorter towards the top - a narrow pyramid when young, an oval later (0.45-0.6 as wide as high), bare to 3-5 m;
		// star leaves, a bright green.
		Model Sweetgum(uint32_t seed)
		{
			Random rnd(seed);
			Model out;
			const float H = rnd.Range(14, 24), W = H * rnd.Range(0.45f, 0.6f), base = rnd.Range(3.0f, 5.0f);
			std::vector<std::pair<Limb, Texture>> limbs;
			std::vector<Leaf> leaves;
			const float tall = H * 0.93f + 0.3f;
			Limb trunk = Grow({0, 0, -0.3f}, {rnd.Range(-0.03f, 0.03f), rnd.Range(-0.03f, 0.03f), 1}, {0, 0, 1}, 0.02f, tall, 8, rnd.Range(0.3f, 0.42f), 0.05f,
			    rnd, 0.03f);
			limbs.push_back({trunk, kBarkOak});
			const P3 middle{0, 0, base + (H - base) * 0.45f};
			for (float z = base; z < H - 1.2f; z += rnd.Range(0.8f, 1.3f))
			{
				const float v = (z - base) / (H - base);
				const P3 at = At(trunk, (z + 0.3f) / tall);
				const int count = 2 + static_cast<int>(rnd.Next() % 2);
				const float a0 = rnd.Range(0, 2 * kPi);
				for (int k = 0; k < count; ++k)
				{
					const float a = a0 + 2 * kPi * k / count + rnd.Range(-0.5f, 0.5f);
					const float reach = W * 0.5f * std::pow(1 - v, 0.55f) * rnd.Range(0.7f, 1.05f) + 0.5f;
					const P3 dir{std::cos(a), std::sin(a), rnd.Range(0.2f, 0.6f)};
					Limb br = Grow(at, dir, {std::cos(a), std::sin(a), 0.3f}, 0.1f, reach, 4, 0.1f * (1 - v * 0.7f), 0.02f, rnd, 0.12f);
					limbs.push_back({br, kBarkOak});
					for (float t = 0.35f; t <= 1.0f; t += 0.2f)
						Cluster(leaves, At(br, t), middle, 3, 0.9f, rnd.Range(1.0f, 1.4f), kGumLeaves, rnd, 0.2f);
				}
			}
			// (a looser shell, rounder below: an oval more than a fir - the branches seen through it)
			for (int k = 0; k < 220; ++k)
			{
				const float a = rnd.Range(0, 2 * kPi), v = rnd.Unit();
				const float r = (W * 0.5f * std::pow(1 - v, 0.55f) + 0.4f) * rnd.Range(0.75f, 1.05f);
				Cluster(leaves, {std::cos(a) * r, std::sin(a) * r, base + 0.5f + (H - base - 0.5f) * v}, middle, 1, 0.3f, rnd.Range(1.0f, 1.35f), kGumLeaves, rnd, 0.0f);
			}
			Finish(out, limbs, leaves);
			out.height = H, out.width = W, out.trunk = trunk.r[0];
			return out;
		}

		// A royal palm (Miami's avenues' and yards'): a smooth pale grey trunk like a concrete column, flared at its foot and
		// swollen a little a third of the way up, a glossy green crownshaft 2-2.6 m long on it, and thirteen to seventeen
		// feather fronds 3.6-4.6 m long arching out of its top - the young ones up, the old hanging.
		Model RoyalPalm(uint32_t seed)
		{
			Random rnd(seed);
			Model out;
			const float H = rnd.Range(14, 22), shaft = rnd.Range(2.0f, 2.6f), frond = rnd.Range(3.6f, 4.6f);
			const float column = H - shaft - frond * 0.35f; // (the grey trunk's height: the crownshaft on it, the upright fronds over that)
			std::vector<std::pair<Limb, Texture>> limbs;
			Limb trunk = Grow({0, 0, -0.3f}, {rnd.Range(-0.03f, 0.03f), rnd.Range(-0.03f, 0.03f), 1}, {0, 0, 1}, 0.01f, column + 0.3f, 10, 0.32f, 0.24f, rnd, 0.01f);
			const float r0 = rnd.Range(0.3f, 0.36f);
			for (size_t i = 0; i < trunk.r.size(); ++i)
			{
				const float t = static_cast<float>(i) / static_cast<float>(trunk.r.size() - 1);
				trunk.r[i] = r0 * (1 + 0.25f * std::max(0.0f, 1 - t * 8) + 0.15f * std::exp(-std::pow((t - 0.38f) / 0.18f, 2.0f)) - 0.3f * t);
			}
			limbs.push_back({trunk, kBarkRoyal});
			const P3 top = trunk.p.back();
			const Limb crownshaft = Grow(top, Sub(top, trunk.p[trunk.p.size() - 2]), {0, 0, 1}, 0.0f, shaft, 3, trunk.r.back() * 1.05f, trunk.r.back() * 0.8f, rnd, 0.005f);
			limbs.push_back({crownshaft, kCrownshaft});
			const P3 crown = crownshaft.p.back(), middle{crown[0], crown[1], crown[2] + 0.5f};
			std::vector<std::array<float, 6>> fronds;
			const int n = 13 + static_cast<int>(rnd.Next() % 5);
			for (int k = 0; k < n; ++k)
			{
				const float age = static_cast<float>(k) / static_cast<float>(n - 1); // (0 the youngest - upright -, 1 the oldest - hanging)
				const float az = k * 2.39996f + rnd.Range(-0.2f, 0.2f);
				const float rise = 1.25f - 1.9f * age + rnd.Range(-0.1f, 0.1f), droop = 0.6f + 0.9f * age;
				fronds.push_back({az, rise, droop, frond * rnd.Range(0.85f, 1.05f) * (age < 0.15f ? 0.75f : 1.0f), rnd.Range(1.5f, 1.9f), static_cast<float>(rnd.Next() % 4)});
			}
			FinishPalm(out, limbs, fronds, crown, middle, 0.35f, {});
			out.height = crown[2] + frond * 0.45f, out.width = 2 * frond * 0.9f, out.trunk = trunk.r[0];
			return out;
		}

		// A coconut palm (the beaches' and the older yards'): a grey-brown ringed trunk leaning out 10-25 degrees and curving
		// back up, swollen at its foot, a crown of twenty-odd feather fronds 3.8-5 m long, most of them drooping.
		Model CoconutPalm(uint32_t seed)
		{
			Random rnd(seed);
			Model out;
			const float H = rnd.Range(9, 16), frond = rnd.Range(3.8f, 5.0f), lean = rnd.Range(0.18f, 0.45f), az = rnd.Range(0, 2 * kPi);
			std::vector<std::pair<Limb, Texture>> limbs;
			Limb trunk = Grow({0, 0, -0.3f}, {std::cos(az) * lean, std::sin(az) * lean, 1}, {-std::cos(az) * 0.3f, -std::sin(az) * 0.3f, 1}, 0.035f,
			    (H - frond * 0.2f) / std::cos(lean * 0.6f), 12, 0.3f, 0.17f, rnd, 0.01f);
			for (size_t i = 0; i < trunk.r.size(); ++i)
			{
				const float t = static_cast<float>(i) / static_cast<float>(trunk.r.size() - 1);
				trunk.r[i] = 0.3f * (1 + 0.35f * std::max(0.0f, 1 - t * 6)) * (1 - 0.42f * t);
			}
			limbs.push_back({trunk, kBarkCoconut});
			const P3 crown = trunk.p.back(), middle{crown[0], crown[1], crown[2] + 0.3f};
			std::vector<std::array<float, 6>> fronds;
			const int n = 20 + static_cast<int>(rnd.Next() % 7);
			for (int k = 0; k < n; ++k)
			{
				const float age = static_cast<float>(k) / static_cast<float>(n - 1);
				const float a = k * 2.39996f + rnd.Range(-0.25f, 0.25f);
				const float rise = 0.9f - 1.6f * age + rnd.Range(-0.15f, 0.15f), droop = 1.0f + 0.9f * age;
				fronds.push_back({a, rise, droop, frond * rnd.Range(0.8f, 1.05f), rnd.Range(1.6f, 2.1f), static_cast<float>(rnd.Next() % 4)});
			}
			FinishPalm(out, limbs, fronds, crown, middle, 0.25f, {});
			out.height = crown[2] + frond * 0.35f, out.width = 2 * frond * 0.85f + std::hypot(crown[0], crown[1]), out.trunk = trunk.r[0];
			return out;
		}

		// A cabbage palm (the sabal, Florida's own): a straight trunk in the criss-cross boots of old leaf bases, a round head
		// of thirty-odd fan leaves on stalks 0.8-1.3 m long out every way, the lower ones hanging - or, two in three, "hurricane
		// cut" as the towns keep them: the lower leaves trimmed off, the head a brush of the upper ones.
		Model SabalPalm(uint32_t seed)
		{
			Random rnd(seed);
			Model out;
			const float H = rnd.Range(8, 14), head = rnd.Range(2.0f, 2.6f);
			std::vector<std::pair<Limb, Texture>> limbs;
			std::vector<Leaf> fans;
			const Limb trunk = Grow({0, 0, -0.3f}, {rnd.Range(-0.05f, 0.05f), rnd.Range(-0.05f, 0.05f), 1}, {0, 0, 1}, 0.02f, H - head + 0.3f, 8, rnd.Range(0.22f, 0.28f),
			    0.22f, rnd, 0.015f);
			limbs.push_back({trunk, kBarkSabal});
			const P3 crown = trunk.p.back(), middle{crown[0], crown[1], crown[2] + 0.4f};
			// (the head's core: the leaf bases' mass the stalks come out of, narrowing up - the trunk's top had been a cut
			// stump in the head further off, where the stalks are not drawn)
			limbs.push_back({Grow(crown, {0, 0, 1}, {0, 0, 1}, 0.0f, 0.8f, 2, trunk.r.back() * 1.3f, 0.08f, rnd, 0.01f), kBarkSabal});
			const bool trimmed = rnd.Next() % 3 != 0;
			const int n = (trimmed ? 26 : 34) + static_cast<int>(rnd.Next() % 8);
			// (the spin that turns a card's picture's up to `up`: its sideways and up as Card makes them)
			const auto spinTo = [](const P3& facing, const P3& up) {
				const P3 f = Unit3(facing);
				const P3 u = Unit3(Cross(f, std::fabs(f[2]) < 0.95f ? P3{0, 0, 1} : P3{1, 0, 0}));
				P3 v = Cross(u, f);
				if (v[2] < 0)
					v = Mul(v, -1);
				return std::atan2(-Dot(up, u), Dot(up, v));
			};
			for (int k = 0; k < n; ++k)
			{
				// (the stalks' directions spread over a sphere, from straight up to well down - none straight down)
				const float a = k * 2.39996f, z = 1 - (trimmed ? 1.15f : 1.6f) * (k + 0.5f) / n, rr = std::sqrt(std::max(0.0f, 1 - z * z));
				const P3 dir{std::cos(a) * rr, std::sin(a) * rr, z};
				// (4.5 cm at the base: drawn at the second level too - the fans had floated round a hole there)
				const Limb stalk = Grow(crown, dir, {0, 0, -1}, 0.05f, rnd.Range(0.8f, 1.3f), 2, 0.045f, 0.025f, rnd, 0.05f);
				limbs.push_back({stalk, kBarkSabal});
				const P3 end = stalk.p.back(), outward = Unit3(Sub(end, crown));
				// the blade spreads on out of the stalk's end as a hand from its arm - the fan's hub (the picture's foot) there,
				// the picture's up along the stalk -, its upper side to the sky, rolled round the stalk a little (each fan had
				// faced out from the crown: the head a ball of discs, "a lollipop")
				P3 face = Sub(P3{0, 0, 1}, Mul(outward, outward[2]));
				if (Dot(face, face) < 0.04f)
					face = {-std::sin(a), std::cos(a), 0};
				face = Unit3(face);
				const float roll = rnd.Range(-0.6f, 0.6f), size = rnd.Range(1.2f, 1.6f);
				face = Add(Mul(face, std::cos(roll)), Mul(Cross(outward, face), std::sin(roll)));
				fans.push_back({Add(end, Mul(outward, size * 0.38f)), face, Sub(end, middle), spinTo(face, outward), size, size, static_cast<int>(rnd.Next() % 4), kFanFrond,
				    false});
			}
			FinishPalm(out, limbs, {}, crown, middle, 0, fans);
			out.height = crown[2] + 2.2f, out.width = 5.0f, out.trunk = trunk.r[0];
			return out;
		}

		// pictures: a pixel's colour scaled
		std::array<uint8_t, 4> Shade(std::array<float, 3> c, float k, uint8_t a = 255)
		{
			return {static_cast<uint8_t>(std::clamp(c[0] * k, 0.0f, 255.0f)), static_cast<uint8_t>(std::clamp(c[1] * k, 0.0f, 255.0f)),
			    static_cast<uint8_t>(std::clamp(c[2] * k, 0.0f, 255.0f)), a};
		}

		float Hash01(int x, int y, uint32_t s)
		{
			uint32_t h = static_cast<uint32_t>(x) * 374761393u + static_cast<uint32_t>(y) * 668265263u + s * 2246822519u;
			h = (h ^ (h >> 13)) * 1274126177u;
			return ((h ^ (h >> 16)) & 0xFFFFFF) / 16777216.0f;
		}

		// smooth noise over a lattice of `cell` pixels
		float Noise(float x, float y, float cell, uint32_t s)
		{
			const float fx = x / cell, fy = y / cell;
			const int ix = static_cast<int>(std::floor(fx)), iy = static_cast<int>(std::floor(fy));
			const float u = fx - ix, v = fy - iy, su = u * u * (3 - 2 * u), sv = v * v * (3 - 2 * v);
			const float a = Hash01(ix, iy, s), b = Hash01(ix + 1, iy, s), c = Hash01(ix, iy + 1, s), d = Hash01(ix + 1, iy + 1, s);
			return a + (b - a) * su + (c - a) * sv + (a - b - c + d) * su * sv;
		}

		// a filled ellipse (anti-aliased only by its own size: the cut-out keeps alpha over half)
		void Ellipse(texture::Image& im, float cx, float cy, float rx, float ry, float angle, std::array<uint8_t, 4> colour, int x0, int y0, int x1, int y1)
		{
			const float cs = std::cos(angle), sn = std::sin(angle), r = std::max(rx, ry);
			for (int y = std::max(y0, static_cast<int>(cy - r - 1)); y <= std::min(y1 - 1, static_cast<int>(cy + r + 1)); ++y)
				for (int x = std::max(x0, static_cast<int>(cx - r - 1)); x <= std::min(x1 - 1, static_cast<int>(cx + r + 1)); ++x)
				{
					const float dx = x + 0.5f - cx, dy = y + 0.5f - cy;
					const float u = (dx * cs + dy * sn) / rx, v = (-dx * sn + dy * cs) / ry;
					if (u * u + v * v <= 1)
						im.pixels[static_cast<size_t>(y) * im.width + x] = colour;
				}
		}

		void Line(texture::Image& im, float x0, float y0, float x1, float y1, float width, std::array<uint8_t, 4> colour, int bx0, int by0, int bx1, int by1)
		{
			const float len = std::hypot(x1 - x0, y1 - y0);
			const int steps = std::max(1, static_cast<int>(len * 2));
			for (int i = 0; i <= steps; ++i)
			{
				const float t = static_cast<float>(i) / steps;
				Ellipse(im, x0 + (x1 - x0) * t, y0 + (y1 - y0) * t, width / 2 + 0.3f, width / 2 + 0.3f, 0, colour, bx0, by0, bx1, by1);
			}
		}

		// A palm's leaflet: a blade from `a` through the bend `b` to its tip `c`, `width` at its widest a little way out,
		// tapering to its point; darker at the rachis, paler and yellower towards the tip, the tip's last fifth brown when
		// `dry`.
		void Blade(texture::Image& im, std::array<float, 2> a, std::array<float, 2> b, std::array<float, 2> c, float width, std::array<float, 3> base,
		    bool dry, int bx0, int by0, int bx1, int by1)
		{
			const float l1 = std::hypot(b[0] - a[0], b[1] - a[1]), l2 = std::hypot(c[0] - b[0], c[1] - b[1]), len = l1 + l2;
			const int steps = std::max(2, static_cast<int>(len * 2));
			for (int i = 0; i <= steps; ++i)
			{
				const float t = static_cast<float>(i) / steps, d = t * len;
				const float x = d < l1 ? a[0] + (b[0] - a[0]) * d / l1 : b[0] + (c[0] - b[0]) * (d - l1) / std::max(1e-3f, l2);
				const float y = d < l1 ? a[1] + (b[1] - a[1]) * d / l1 : b[1] + (c[1] - b[1]) * (d - l1) / std::max(1e-3f, l2);
				const float w = width * (t < 0.12f ? 0.55f + 3.75f * t : 1 - 0.8f * std::pow((t - 0.12f) / 0.88f, 1.4f));
				std::array<float, 3> k{};
				for (int q = 0; q < 3; ++q)
					k[q] = base[q] * (0.8f + 0.32f * t) + (q < 2 ? 10.0f : 0.0f) * t; // (paler, a touch yellower, out to the tip)
				if (dry && t > 0.8f)
				{
					const float m = std::min(1.0f, (t - 0.8f) / 0.1f);
					const std::array<float, 3> brown{142, 118, 76};
					for (int q = 0; q < 3; ++q)
						k[q] += (brown[q] - k[q]) * m;
				}
				Ellipse(im, x, y, w / 2 + 0.3f, w / 2 + 0.3f, 0, Shade(k, 1.0f), bx0, by0, bx1, by1);
			}
		}
	}

	texture::Image Picture(Texture t)
	{
		const bool atlas = Cutout(t) && t != kMoss;
		texture::Image im(t == kMoss ? 256 : 512, 512, {0, 0, 0, 0});
		if (!Cutout(t))
		{
			// barks: a live oak's dark grey ridged bark, a pine's red-brown plates, a crape myrtle's smooth mottled tan
			for (int y = 0; y < im.height; ++y)
				for (int x = 0; x < im.width; ++x)
				{
					auto& p = im.pixels[static_cast<size_t>(y) * im.width + x];
					if (t == kBarkOak)
					{
						// (an oak's bark: dark grey ridges, long and irregular, up and down, the furrows between them
						// wandering and joining - noise stretched five times along the trunk: no stripes of one width, no
						// bricks of rows of cracks)
						const float ridge = 0.65f * Noise(x, y * 0.2f, 16, 3) + 0.35f * Noise(x, y * 0.25f, 6, 4);
						const float k = ridge < 0.42f ? 0.42f + 0.4f * ridge : 0.7f + 0.35f * Noise(x, y * 0.5f, 10, 5);
						p = Shade({60, 56, 52}, k * (0.9f + 0.2f * Noise(x, y, 3, 6)));
					}
					else if (t == kBarkPine)
					{
						// (long irregular plates between dark vertical fissures that wander: no brick bond)
						const float wander = 22 * Noise(x * 0.3f, y, 60, 11) + 8 * Noise(x, y, 14, 12);
						const float fissure = std::fabs(std::sin((x + wander) * 2 * kPi / 34));
						const float plate = 0.8f + 0.35f * Noise(x, y * 0.25f, 20, 13);
						const float k = (fissure < 0.22f ? 0.45f + fissure : plate) * (0.88f + 0.22f * Noise(x, y, 4, 8));
						p = Shade({100, 82, 70}, k);
					}
					else if (t == kBarkSmooth)
					{
						// (a magnolia's: smooth dark grey-brown, faint blotches - a paler one had read as white-washed)
						const float k = 0.85f + 0.2f * Noise(x, y * 0.6f, 30, 14);
						p = Shade({86, 80, 72}, k);
					}
					else if (t == kBarkRoyal)
					{
						// (a royal palm's: smooth grey, like concrete - faint rings of old leaf scars every 9 cm or so, streaks up
						// and down where the rain runs, darker blotches and a green-grey film here and there: one even pale grey
						// had read as "paper-like")
						const float ring = std::fabs(std::sin((y + 3 * Noise(x, y, 40, 21)) * 2 * kPi / 14));
						const float streak = Noise(x, y * 0.08f, 9, 22), blotch = Noise(x, y, 70, 31), film = Noise(x, y, 110, 32);
						const float k = (ring < 0.12f ? 0.91f : 1.0f) * (0.86f + 0.16f * streak) * (blotch > 0.6f ? 0.88f : 1.0f);
						const std::array<float, 3> grey = film > 0.62f ? std::array<float, 3>{150, 156, 140} : std::array<float, 3>{166, 164, 158};
						p = Shade(grey, k);
					}
					else if (t == kBarkCoconut)
					{
						// (a coconut palm's: grey-brown, the leaf scars' rings every 12-16 cm faint and uneven, broken here and
						// there, weathered paler patches and darker ones - sharp rings and fine cracks up and down at full
						// contrast had read as a woven grid, "a fine square grid texture")
						const float ring = std::fabs(std::sin((y + 9 * Noise(x * 0.5f, y * 0.3f, 50, 23)) * 2 * kPi / 22));
						const bool scar = ring < 0.16f && Noise(x, y, 18, 29) > 0.3f;
						const float patch = Noise(x, y, 80, 30), grain = Noise(x, y * 0.4f, 7, 25);
						std::array<float, 3> c{136, 126, 110};
						if (patch > 0.62f) // (weathered pale grey)
							c = {158, 154, 144};
						else if (patch < 0.3f)
							c = {116, 106, 92};
						p = Shade(c, (scar ? 0.86f : 1.0f) * (0.94f + 0.1f * grain));
					}
					else if (t == kBarkSabal)
					{
						// (a cabbage palm's boots: the old leaf bases split into a criss-cross lattice, dark between them)
						const float d1 = std::fabs(std::sin((x + y * 0.8f) * 2 * kPi / 46)), d2 = std::fabs(std::sin((x - y * 0.8f) * 2 * kPi / 46));
						p = Shade({116, 100, 80}, (std::max(d1, d2) > 0.55f ? 0.95f : 0.5f) * (0.85f + 0.25f * Noise(x, y, 8, 26)));
					}
					else if (t == kCrownshaft)
					{
						// (a royal palm's crownshaft: smooth glossy green, faint lines up and down)
						p = Shade({86, 120, 70}, 0.9f + 0.12f * Noise(x * 3.0f, y * 0.1f, 8, 27));
					}
					else
					{
						const float k = 0.85f + 0.25f * Noise(x, y, 24, 9) - 0.15f * (Noise(x, y, 60, 10) > 0.65f ? 1 : 0);
						p = Shade({176, 150, 120}, k);
					}
				}
			return im;
		}
		// the four clusters of an atlas (or the moss's one strip)
		const int cells = atlas ? 4 : 1;
		for (int cell = 0; cell < cells; ++cell)
		{
			const int x0 = atlas ? (cell % 2) * 256 : 0, y0 = atlas ? (cell / 2) * 256 : 0, x1 = x0 + (atlas ? 256 : im.width), y1 = y0 + (atlas ? 256 : im.height);
			Random rnd(static_cast<uint32_t>(t) * 977 + cell * 31 + 5);
			const float cx = (x0 + x1) / 2.0f, cy = (y0 + y1) / 2.0f;
			if (t == kOakLeaves || t == kMyrtle)
			{
				// leaves: small ellipses in a ragged round cluster, darker inside, a few twigs; a crape myrtle's with its
				// flowers' panicles over them (the cell's colour)
				for (int k = 0; k < 7; ++k)
				{
					const float a = rnd.Range(0, 2 * kPi);
					Line(im, cx, cy + 60, cx + std::cos(a) * 90, cy + std::sin(a) * 70, 2.2f, {74, 60, 46, 255}, x0, y0, x1, y1);
				}
				const int leaves = t == kOakLeaves ? 900 : 420;
				// (lobes: five to seven bunches of leaves - a ragged, lumpy outline, not a disc)
				std::array<std::array<float, 3>, 7> lobes{};
				const int lobeCount = 5 + static_cast<int>(rnd.Next() % 3);
				for (int q = 0; q < lobeCount; ++q)
				{
					const float a = rnd.Range(0, 2 * kPi), d = q ? rnd.Range(35, 75) : 0;
					lobes[static_cast<size_t>(q)] = {cx + std::cos(a) * d, cy + std::sin(a) * d * 0.85f, q ? rnd.Range(30, 52) : 58};
				}
				for (int k = 0; k < leaves; ++k)
				{
					const auto& lobe = lobes[rnd.Next() % static_cast<uint32_t>(lobeCount)];
					const float a = rnd.Range(0, 2 * kPi), d = lobe[2] * std::sqrt(rnd.Unit());
					const float x = lobe[0] + std::cos(a) * d, y = lobe[1] + std::sin(a) * d * 0.9f;
					const float lit = 0.65f + 0.4f * (1 - (y - y0) / 256.0f) * rnd.Range(0.6f, 1.0f);
					const std::array<float, 3> base = t == kOakLeaves ? std::array<float, 3>{rnd.Range(26, 40), rnd.Range(38, 54), rnd.Range(22, 32)}
					                                                  : std::array<float, 3>{rnd.Range(46, 62), rnd.Range(70, 88), rnd.Range(34, 46)};
					Ellipse(im, x, y, rnd.Range(5.5f, 9.5f), rnd.Range(3.0f, 4.6f), rnd.Range(0, kPi), Shade(base, lit), x0, y0, x1, y1);
				}
				if (t == kMyrtle)
				{
					const std::array<float, 3> blooms[4] = {{196, 36, 64}, {226, 104, 150}, {236, 232, 226}, {168, 112, 196}};
					for (int k = 0; k < 9; ++k)
					{
						const float a = rnd.Range(0, 2 * kPi), d = rnd.Range(0, 80);
						const float px = cx + std::cos(a) * d, py = cy + std::sin(a) * d * 0.8f - 20;
						for (int q = 0; q < 70; ++q)
						{
							const float b = rnd.Range(0, 2 * kPi), e = 26 * std::sqrt(rnd.Unit());
							Ellipse(im, px + std::cos(b) * e * 0.7f, py + std::sin(b) * e, rnd.Range(2.2f, 3.6f), rnd.Range(2.0f, 3.2f), rnd.Range(0, kPi),
							    Shade(blooms[cell], rnd.Range(0.75f, 1.1f)), x0, y0, x1, y1);
						}
					}
				}
			}
			else if (t == kMagnoliaLeaves || t == kGumLeaves)
			{
				// twigs, then the leaves in a ragged cluster: a magnolia's big dark glossy ellipses (a few showing their rusty
				// undersides; no flowers - a crown dotted white had read as one generic flowering tree), a sweetgum's
				// five-pointed stars of a brighter green
				for (int k = 0; k < 6; ++k)
				{
					const float a = rnd.Range(0, 2 * kPi);
					Line(im, cx, cy + 60, cx + std::cos(a) * 85, cy + std::sin(a) * 65, 2.4f, {80, 66, 52, 255}, x0, y0, x1, y1);
				}
				const bool magnolia = t == kMagnoliaLeaves;
				std::array<std::array<float, 3>, 7> lobes{};
				const int lobeCount = 5 + static_cast<int>(rnd.Next() % 3);
				for (int q = 0; q < lobeCount; ++q)
				{
					const float a = rnd.Range(0, 2 * kPi), d = q ? rnd.Range(35, 75) : 0;
					lobes[static_cast<size_t>(q)] = {cx + std::cos(a) * d, cy + std::sin(a) * d * 0.85f, q ? rnd.Range(30, 52) : 58};
				}
				const int count = magnolia ? 330 : 260;
				for (int k = 0; k < count; ++k)
				{
					const auto& lobe = lobes[rnd.Next() % static_cast<uint32_t>(lobeCount)];
					const float a = rnd.Range(0, 2 * kPi), d = lobe[2] * std::sqrt(rnd.Unit());
					const float x = lobe[0] + std::cos(a) * d, y = lobe[1] + std::sin(a) * d * 0.9f;
					const float lit = 0.6f + 0.45f * (1 - (y - y0) / 256.0f) * rnd.Range(0.6f, 1.0f);
					if (magnolia)
					{
						const bool under = rnd.Unit() < 0.03f;
						const std::array<float, 3> base = under ? std::array<float, 3>{rnd.Range(96, 118), rnd.Range(64, 76), rnd.Range(34, 44)}
						                                        : std::array<float, 3>{rnd.Range(18, 30), rnd.Range(38, 54), rnd.Range(16, 26)};
						const float rx = rnd.Range(10.0f, 15.0f), ry = rnd.Range(4.5f, 6.5f), ang = rnd.Range(0, kPi);
						Ellipse(im, x, y, rx, ry, ang, Shade(base, lit), x0, y0, x1, y1);
						if (!under && rnd.Unit() < 0.5f) // (the gloss: a pale streak along the leaf)
							Ellipse(im, x, y, rx * 0.6f, 1.2f, ang, Shade(base, lit * 2.2f), x0, y0, x1, y1);
					}
					else
					{
						const std::array<float, 3> base{rnd.Range(48, 64), rnd.Range(90, 110), rnd.Range(30, 42)};
						const float turn = rnd.Range(0, 2 * kPi), size = rnd.Range(6.0f, 9.0f);
						for (int point = 0; point < 5; ++point)
						{
							const float b = turn + point * 2 * kPi / 5;
							Ellipse(im, x + std::cos(b) * size * 0.5f, y + std::sin(b) * size * 0.5f, size * 0.6f, size * 0.22f, b, Shade(base, lit), x0, y0, x1, y1);
						}
					}
				}
			}
			else if (t == kPineNeedles)
			{
				// tufts of needles out from a twig: thin strokes fanning out all round
				for (int tuft = 0; tuft < 6; ++tuft)
				{
					const float a = rnd.Range(0, 2 * kPi), d = rnd.Range(10, 70);
					const float tx = cx + std::cos(a) * d, ty = cy + std::sin(a) * d * 0.8f;
					Line(im, cx, cy + 40, tx, ty, 2.0f, {80, 58, 40, 255}, x0, y0, x1, y1);
					for (int k = 0; k < 110; ++k)
					{
						const float b = rnd.Range(0, 2 * kPi), len = rnd.Range(22, 46);
						const float lit = 0.75f + 0.45f * rnd.Unit();
						Line(im, tx, ty, tx + std::cos(b) * len, ty + std::sin(b) * len * 0.85f, rnd.Range(1.0f, 1.8f),
						    Shade({rnd.Range(48, 64), rnd.Range(70, 92), rnd.Range(32, 44)}, lit), x0, y0, x1, y1);
					}
				}
			}
			else if (t == kPalmFrond)
			{
				// a feather frond: its rachis from the cell's foot to its top, leaflets out of it on both sides - longest past its
				// middle, slanting towards the tip, long narrow blades tapering to their points, paler towards them; olive, one
				// in fifteen torn off, some tips dry (each blade one flat colour, 0.85-1.15 apart, a saturated yellow-green: the
				// reviewers' "bright yellow-green striped frond cards"); the last cell an older, yellower frond
				const bool old = cell == 3;
				const float bx = cx + rnd.Range(-6, 6);
				Line(im, bx, y1 - 4.0f, cx, y0 + 6.0f, 3.0f, {112, 110, 74, 255}, x0, y0, x1, y1);
				// (apart, the sky between them - packed close they had made one oval leaf -, each slanting towards the tip and
				// its outer third drooping)
				for (float s = 0.03f; s < 0.98f; s += 0.02f)
				{
					const float py = y1 - 4 - (y1 - y0 - 10) * s, px = bx + (cx - bx) * s;
					const float len = 118 * std::sin(kPi * std::min(1.0f, 0.08f + 0.95f * s)) * rnd.Range(0.85f, 1.05f);
					for (const float sideSign : {-1.0f, 1.0f})
					{
						const float ang = 0.6f + rnd.Range(-0.12f, 0.12f);
						const float lit = rnd.Range(0.93f, 1.07f);
						const std::array<float, 3> base = old ? std::array<float, 3>{rnd.Range(126, 146) * lit, rnd.Range(118, 134) * lit, rnd.Range(60, 72) * lit}
						                                     : std::array<float, 3>{rnd.Range(66, 78) * lit, rnd.Range(86, 98) * lit, rnd.Range(42, 50) * lit};
						const bool torn = rnd.Unit() < 0.07f, dry = rnd.Unit() < (old ? 0.5f : 0.1f);
						const float width = rnd.Range(2.3f, 3.1f);
						const float mx = px + sideSign * len * 0.65f * std::cos(ang), my = py - len * 0.65f * std::sin(ang) * 0.6f;
						if (!torn)
							Blade(im, {px, py}, {mx, my}, {mx + sideSign * len * 0.35f * std::cos(ang * 0.3f), my + len * 0.12f}, width, base, dry, x0, y0, x1, y1);
					}
				}
			}
			else if (t == kFanFrond)
			{
				// a cabbage palm's fan: forty-odd narrow segments out of a hub near the cell's foot over a little more than a half
				// circle, their split tips drooping
				const float hx = cx, hy = y1 - 30.0f;
				for (int k = 0; k < 44; ++k)
				{
					const float a = kPi * (-0.1f + 1.2f * k / 43.0f) + rnd.Range(-0.03f, 0.03f);
					const float len = rnd.Range(150, 200) * (0.75f + 0.25f * std::sin(kPi * k / 43.0f));
					const float ex = hx + std::cos(a) * len * 0.62f, ey = hy - std::sin(a) * len * 0.95f;
					const std::array<float, 3> base{rnd.Range(50, 66), rnd.Range(86, 104), rnd.Range(40, 54)};
					Line(im, hx, hy, ex, ey, rnd.Range(4.0f, 6.0f), Shade(base, rnd.Range(0.85f, 1.1f)), x0, y0, x1, y1);
					Line(im, ex, ey, ex + rnd.Range(-10, 10), ey + rnd.Range(8, 22), 2.0f, Shade(base, 0.9f), x0, y0, x1, y1);
				}
			}
			else
			{
				// Spanish moss: grey-green strands hanging from the top edge, waving, thinning out downwards (55 thin dark ones had
				// all but vanished at 10 m; 100-140 thick pale ones on wide cards were "flat opaque paper strips" at 20 m: 60 of
				// them, on narrower cards)
				for (int k = 0; k < 60; ++k)
				{
					// (a beard: the strands bunched in the middle and longest there, the sides short)
					const float g = (rnd.Unit() + rnd.Unit() + rnd.Unit()) / 3 - 0.5f;
					float x = im.width / 2.0f + g * (im.width - 30.0f);
					const float len = (0.25f + 0.75f * (1 - std::fabs(g) * 2)) * rnd.Range(0.6f, 1.0f) * im.height, phase = rnd.Range(0, 2 * kPi),
					            sway = rnd.Range(3, 12);
					const std::array<float, 3> c{rnd.Range(118, 138), rnd.Range(124, 142), rnd.Range(108, 124)};
					for (float y = rnd.Range(0, 20); y < len; y += 1.0f)
					{
						const float w = 2.6f * (1 - y / len) + 1.0f;
						const float px = x + std::sin(y / 40 + phase) * sway;
						Ellipse(im, px, y, w / 2 + 0.2f, 0.8f, 0, Shade(c, rnd.Range(0.85f, 1.05f)), x0, y0, x1, y1);
						if (rnd.Unit() < 0.02f)
							Line(im, px, y, px + rnd.Range(-14, 14), y + rnd.Range(10, 30), 1.0f, Shade(c, 0.9f), x0, y0, x1, y1);
					}
				}
			}
		}
		return im;
	}

	namespace
	{
		// A shrub: a mound of leaf cards (1.2-2.4 m tall, 1.3-1.8 times as wide), a few stems inside it.
		Model Shrub(uint32_t seed)
		{
			Random rnd(seed);
			Model out;
			const float H = rnd.Range(1.2f, 2.4f), W = H * rnd.Range(1.3f, 1.8f);
			std::vector<std::pair<Limb, Texture>> limbs;
			std::vector<Leaf> leaves;
			const P3 middle{0, 0, H * 0.45f};
			for (int k = 0; k < 4; ++k)
			{
				const float a = rnd.Range(0, 2 * kPi);
				limbs.push_back({Grow({0, 0, -0.1f}, {std::cos(a) * 0.5f, std::sin(a) * 0.5f, 1}, {0, 0, 1}, 0.05f, H * 0.6f, 2, 0.03f, 0.015f, rnd), kBarkMyrtle});
			}
			// (many small cards, from the ground up: a few big ones lifted off it had read as "blocky floating clumps")
			const int cards = static_cast<int>(62 * W * H);
			for (int k = 0; k < cards; ++k)
			{
				// (on and under the surface of a half ellipsoid)
				const float a = rnd.Range(0, 2 * kPi), up = std::asin(rnd.Range(0.0f, 1.0f)), depth = rnd.Range(0.6f, 1.0f);
				const P3 c{std::cos(a) * std::cos(up) * W * 0.45f * depth, std::sin(a) * std::cos(up) * W * 0.45f * depth, H * 0.04f + std::sin(up) * H * 0.84f * depth};
				Cluster(leaves, c, middle, 1, 0.1f, rnd.Range(0.45f, 0.7f), kOakLeaves, rnd, 0.0f);
			}
			Finish(out, limbs, leaves);
			out.height = H, out.width = W, out.trunk = 0;
			return out;
		}
	}

	Model Make(Species s, uint32_t seed)
	{
		switch (s)
		{
		case Species::LiveOak: return LiveOak(seed);
		case Species::Pine: return Pine(seed);
		case Species::Oak: return Oak(seed);
		case Species::Myrtle: return Myrtle(seed);
		case Species::Magnolia: return Magnolia(seed);
		case Species::Sweetgum: return Sweetgum(seed);
		case Species::RoyalPalm: return RoyalPalm(seed);
		case Species::CoconutPalm: return CoconutPalm(seed);
		case Species::SabalPalm: return SabalPalm(seed);
		default: return Shrub(seed);
		}
	}
}
