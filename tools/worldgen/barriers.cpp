#include "barriers.hpp"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <string>
#include <unordered_map>

#include "convert/resource.hpp"
#include "seeds.hpp"

namespace worldgen::barriers
{
	namespace
	{
		using P2 = std::array<float, 2>;
		using P3 = std::array<float, 3>;
		using Uv = std::array<float, 2>;
		using Colour = std::array<uint8_t, 4>;

		// materials.dat rows
		constexpr uint8_t kConcrete = 1, kChainLink = 61, kRailing = 65, kWood = 70;

		enum class Kind
		{
			ChainLink, // wire mesh on galvanised posts with a top rail
			Boards,    // a board fence: boards on wooden posts
			Railing,   // posts and two rails (split rail, metal railing, handrails, wire fences)
			Wall,      // painted block, 0.2 m thick
			Retaining, // concrete, 0.3 m thick
			Hedge,     // clipped bushes in a row
			GuardRail, // a W-beam on posts
			Bars,      // wrought iron pickets between square posts (the cemeteries', the older houses')
		};

		Kind KindOf(const world::Barrier& b)
		{
			if (b.kind == "hedge")
				return Kind::Hedge;
			if (b.kind == "guard_rail")
				return Kind::GuardRail;
			if (b.kind == "retaining_wall")
				return Kind::Retaining;
			if (b.kind == "wall")
				return Kind::Wall;
			if (b.kind == "handrail")
				return Kind::Railing;
			const std::string& m = b.material;
			if (m == "wood" || m == "wooden" || m == "board" || m == "boards" || m == "privacy" || m == "panel")
				return Kind::Boards;
			if (m == "bars" || m == "metal_bars" || m == "wrought_iron" || m == "iron" || m == "aluminium")
				return Kind::Bars;
			if (m == "railing" || m == "split_rail" || m == "metal_bars" || m == "bars" || m == "pole" || m == "post_and_rail" || m == "barbed_wire" ||
			    m == "electric" || m == "wire")
				return Kind::Railing;
			return Kind::ChainLink; // (chain_link, chain, metal, net and the untagged: the street pictures' commonest)
		}

		// the height where the map gives none (or one out of reason), by kind: a six-foot fence, a four-foot railing, a
		// garden wall, a retaining wall's face, a hedge, a guard rail's beam top
		float HeightOf(const world::Barrier& b, Kind k)
		{
			const float tagged = b.height;
			switch (k)
			{
			case Kind::ChainLink:
			case Kind::Boards:
				return tagged >= 0.6f && tagged <= 4.5f ? tagged : 1.8f;
			case Kind::Railing:
				return tagged >= 0.5f && tagged <= 2.0f ? tagged : 1.1f;
			case Kind::Bars:
				return tagged >= 0.6f && tagged <= 2.5f ? tagged : 1.8f;
			case Kind::Wall:
				return tagged >= 0.4f && tagged <= 6.0f ? tagged : 1.6f;
			case Kind::Retaining:
				return tagged >= 0.3f && tagged <= 4.0f ? tagged : 0.9f;
			case Kind::Hedge:
				return tagged >= 0.5f && tagged <= 4.0f ? tagged : 1.5f;
			case Kind::GuardRail:
				return 0.76f;
			}
			return 1.8f;
		}

		// posts' spacing: ten feet for chain link, eight for boards and railings, a guard rail's 6 ft 3 in; walls in pieces
		float SpacingOf(Kind k)
		{
			switch (k)
			{
			case Kind::ChainLink:
				return 3.05f;
			case Kind::Boards:
			case Kind::Railing:
			case Kind::Bars:
				return 2.44f;
			case Kind::GuardRail:
				return 1.905f;
			case Kind::Hedge:
				return 1.25f; // (the bushes 1.9 m long overlapping: 1.7 m apart they had read as "a row of identical rounded blobs")
			default:
				return 3.0f;
			}
		}

		P2 Left(const P2& d) { return {-d[1], d[0]}; }

		struct Writer
		{
			roads::Collision& col;

			static uint32_t Add(roads::Mesh& m, const P3& p, const P3& n, const P2& t, const Uv& uv, const Colour& colour)
			{
				ydr::Vertex v;
				v.position = p;
				v.normal = n;
				v.tangent = {t[0], t[1], 0, 1};
				v.colour0 = colour;
				v.uv[0] = uv;
				m.vertices.push_back(v);
				return static_cast<uint32_t>(m.vertices.size() - 1);
			}
			// a quad, its corners anticlockwise seen from the side its normal faces
			static void Quad(roads::Mesh& m, const std::array<P3, 4>& q, const P3& n, const P2& t, const std::array<Uv, 4>& uv,
			    const std::array<Colour, 4>& colours = {Colour{255, 255, 255, 255}, Colour{255, 255, 255, 255}, Colour{255, 255, 255, 255},
			        Colour{255, 255, 255, 255}})
			{
				uint32_t i[4];
				for (int k = 0; k < 4; ++k)
					i[k] = Add(m, q[k], n, t, uv[k], colours[k]);
				m.indices.insert(m.indices.end(), {i[0], i[1], i[2], i[0], i[2], i[3]});
			}
			// A vertical face over the ground line a0 -> a1 (bottoms za0, za1, tops ta0, ta1), facing the line's left; `u0`,
			// `u1` its u at a0, a1, its v from `v0` at the bottom by `vPerMetre` up.
			static void Face(roads::Mesh& m, const P2& a0, const P2& a1, float za0, float za1, float ta0, float ta1, float u0, float u1, float v0,
			    float vPerMetre, const Colour& bottom = {255, 255, 255, 255}, const Colour& top = {255, 255, 255, 255})
			{
				const P2 d{a1[0] - a0[0], a1[1] - a0[1]};
				const float l = std::hypot(d[0], d[1]);
				if (l < 1e-4f)
					return;
				const P2 t{d[0] / l, d[1] / l}, n = Left(t);
				Quad(m, {P3{a0[0], a0[1], za0}, P3{a0[0], a0[1], ta0}, P3{a1[0], a1[1], ta1}, P3{a1[0], a1[1], za1}}, {n[0], n[1], 0}, t,
				    {Uv{u0, v0}, Uv{u0, v0 - (ta0 - za0) * vPerMetre}, Uv{u1, v0 - (ta1 - za1) * vPerMetre}, Uv{u1, v0}}, {bottom, top, top, bottom});
			}
			// a box along `d` (half lengths `along` and `across`) from z0 to z1 round c: its four sides (and its top when
			// `cap`), u along each side, v up (both a metre's worth by `scale`)
			static void Box(roads::Mesh& m, const P2& c, const P2& d, float along, float across, float z0, float z1, float scale, bool cap)
			{
				const P2 n = Left(d);
				const std::array<P2, 4> corner{P2{c[0] + d[0] * along + n[0] * across, c[1] + d[1] * along + n[1] * across},
				    P2{c[0] + d[0] * along - n[0] * across, c[1] + d[1] * along - n[1] * across},
				    P2{c[0] - d[0] * along - n[0] * across, c[1] - d[1] * along - n[1] * across},
				    P2{c[0] - d[0] * along + n[0] * across, c[1] - d[1] * along + n[1] * across}}; // (clockwise from above: outward is each side's left)
				float u = 0;
				for (int k = 0; k < 4; ++k)
				{
					const P2 &p = corner[static_cast<size_t>(k)], &q = corner[static_cast<size_t>((k + 1) % 4)];
					const float l = std::hypot(q[0] - p[0], q[1] - p[1]);
					Face(m, p, q, z0, z0, z1, z1, u * scale, (u + l) * scale, 0, scale);
					u += l;
				}
				if (cap)
					Quad(m, {P3{corner[0][0], corner[0][1], z1}, P3{corner[3][0], corner[3][1], z1}, P3{corner[2][0], corner[2][1], z1},
					            P3{corner[1][0], corner[1][1], z1}},
					    {0, 0, 1}, d, {Uv{0, 0}, Uv{along * 2 * scale, 0}, Uv{along * 2 * scale, across * 2 * scale}, Uv{0, across * 2 * scale}});
			}
			void Collide(const P3& a, const P3& b, const P3& c, uint8_t material)
			{
				const uint32_t base = static_cast<uint32_t>(col.vertices.size());
				col.vertices.insert(col.vertices.end(), {a, b, c});
				col.triangles.push_back({base, base + 1, base + 2});
				col.materials.push_back(material);
				col.kinds.push_back(kCollisionKind);
			}
			// a vertical face's collision (both of its triangles; the other side too when `both`)
			void CollideFace(const P2& a0, const P2& a1, float za0, float za1, float ta0, float ta1, uint8_t material, bool both)
			{
				const P3 b0{a0[0], a0[1], za0}, b1{a1[0], a1[1], za1}, t0{a0[0], a0[1], ta0}, t1{a1[0], a1[1], ta1};
				Collide(b0, t0, t1, material), Collide(b0, t1, b1, material);
				if (both)
					Collide(b0, t1, t0, material), Collide(b0, b1, t1, material);
			}
		};

		uint32_t Mix(uint32_t h)
		{
			h ^= h >> 16;
			h *= 0x7feb352du;
			h ^= h >> 15;
			h *= 0x846ca68bu;
			return h ^ (h >> 16);
		}
	}

	void Build(const std::vector<world::Barrier>& list, const std::function<float(float, float)>& ground,
	    const std::function<bool(float, float)>& onRoad, const std::array<float, 4>& box, Meshes& out, roads::Collision& collision,
	    std::vector<props::Prop>& props, Stats& stats)
	{
		Writer w{collision};
		const uint32_t hedgeBush = loader::convert::Joaat("prop_bush_neat_04"); // (3.2 m long along its y, 2.0 wide, 1.5 high)
		for (size_t bi = 0; bi < list.size(); ++bi)
		{
			const world::Barrier& b = list[bi];
			if (b.points.size() < 2)
				continue;
			const Kind kind = KindOf(b);
			const float h = HeightOf(b, kind), spacing = SpacingOf(kind);
			// the line in pieces no longer than the spacing
			std::vector<P2> q{b.points.front()};
			for (size_t k = 0; k + 1 < b.points.size(); ++k)
			{
				const P2 &a = b.points[k], &c = b.points[k + 1];
				const float l = std::hypot(c[0] - a[0], c[1] - a[1]);
				const int n = std::max(1, static_cast<int>(std::ceil(l / spacing - 0.01f)));
				for (int j = 1; j <= n; ++j)
					q.push_back({a[0] + (c[0] - a[0]) * j / n, a[1] + (c[1] - a[1]) * j / n});
			}
			// (which pieces are built: in the box, not over a road)
			std::vector<bool> built(q.size() - 1, false);
			bool any = false;
			for (size_t k = 0; k + 1 < q.size(); ++k)
			{
				const P2 m{(q[k][0] + q[k + 1][0]) / 2, (q[k][1] + q[k + 1][1]) / 2};
				if (m[0] < box[0] || m[1] < box[1] || m[0] >= box[2] || m[1] >= box[3] || std::hypot(q[k + 1][0] - q[k][0], q[k + 1][1] - q[k][1]) < 0.05f)
					continue;
				if (onRoad(m[0], m[1]))
				{
					++stats.crossing;
					continue;
				}
				built[k] = any = true;
			}
			if (!any)
				continue;
			std::vector<float> z(q.size()), s(q.size(), 0.0f);
			for (size_t k = 0; k < q.size(); ++k)
			{
				z[k] = ground(q[k][0], q[k][1]) - 0.05f;
				if (k)
					s[k] = s[k - 1] + std::hypot(q[k][0] - q[k - 1][0], q[k][1] - q[k - 1][1]);
			}
			const auto dirOf = [&](size_t k) {
				const float l = std::max(1e-4f, s[k + 1] - s[k]);
				return P2{(q[k + 1][0] - q[k][0]) / l, (q[k + 1][1] - q[k][1]) / l};
			};
			// a post at a built piece's start, and at its end where no built piece goes on
			const auto posts = [&](auto&& post) {
				for (size_t k = 0; k + 1 < q.size(); ++k)
					if (built[k])
					{
						post(k, dirOf(k));
						if (k + 2 >= q.size() || !built[k + 1])
							post(k + 1, dirOf(k));
					}
			};
			switch (kind)
			{
			case Kind::ChainLink:
			{
				// the wire mesh (the game's chain link fence's mapping: 8 repeats a metre either way; the vertex colours'
				// blue from 34 at the foot to 255 at the top, as its own), a top rail, a post every 10 ft
				const Colour foot{234, 234, 34, 255}, top{234, 234, 255, 255};
				for (size_t k = 0; k + 1 < q.size(); ++k)
				{
					if (!built[k])
						continue;
					const float za = z[k] + 0.03f, zb = z[k + 1] + 0.03f, ta = z[k] + h - 0.02f, tb = z[k + 1] + h - 0.02f;
					Writer::Face(out.mesh, q[k], q[k + 1], za, zb, ta, tb, s[k] * 8, s[k + 1] * 8, 0, 8, foot, top);
					Writer::Face(out.mesh, q[k + 1], q[k], zb, za, tb, ta, s[k + 1] * 8, s[k] * 8, 0, 8, foot, top);
					w.CollideFace(q[k], q[k + 1], za, zb, ta, tb, kChainLink, false);
					// (the top rail: 4 cm round, its three sides seen)
					const P2 d = dirOf(k), n = Left(d);
					constexpr float r = 0.02f;
					const P2 l0{q[k][0] + n[0] * r, q[k][1] + n[1] * r}, l1{q[k + 1][0] + n[0] * r, q[k + 1][1] + n[1] * r};
					const P2 r0{q[k][0] - n[0] * r, q[k][1] - n[1] * r}, r1{q[k + 1][0] - n[0] * r, q[k + 1][1] - n[1] * r};
					Writer::Face(out.steel, r1, r0, tb - r, ta - r, tb + r, ta + r, s[k + 1], s[k], 0, 1);
					Writer::Face(out.steel, l0, l1, ta - r, tb - r, ta + r, tb + r, s[k], s[k + 1], 0, 1);
					Writer::Quad(out.steel, {P3{l0[0], l0[1], ta + r}, P3{r0[0], r0[1], ta + r}, P3{r1[0], r1[1], tb + r}, P3{l1[0], l1[1], tb + r}},
					    {0, 0, 1}, d, {Uv{s[k], 0}, Uv{s[k], 0.04f}, Uv{s[k + 1], 0.04f}, Uv{s[k + 1], 0}});
					++stats.pieces;
				}
				posts([&](size_t k, const P2& d) { Writer::Box(out.steel, q[k], d, 0.03f, 0.03f, z[k] - 0.1f, z[k] + h + 0.04f, 1.0f, true); });
				++stats.fences;
				break;
			}
			case Kind::Boards:
			{
				// boards (the weathered boards' picture turned on its side: its boards stand up, 10 to 1.5 m, its post
				// strip - the last eighth - left out), both faces, on 9 cm posts
				for (size_t k = 0; k + 1 < q.size(); ++k)
				{
					if (!built[k])
						continue;
					const P2 n = Left(dirOf(k));
					constexpr float half = 0.012f;
					const P2 a0{q[k][0] + n[0] * half, q[k][1] + n[1] * half}, a1{q[k + 1][0] + n[0] * half, q[k + 1][1] + n[1] * half};
					const P2 b0{q[k][0] - n[0] * half, q[k][1] - n[1] * half}, b1{q[k + 1][0] - n[0] * half, q[k + 1][1] - n[1] * half};
					const float za = z[k] + 0.05f, zb = z[k + 1] + 0.05f, ta = z[k] + h, tb = z[k + 1] + h;
					const auto boards = [&](roads::Mesh& m, const P2& p0, const P2& p1, float s0, float s1, float zp0, float zp1, float tp0, float tp1) {
						const P2 d{p1[0] - p0[0], p1[1] - p0[1]};
						const float l = std::hypot(d[0], d[1]);
						const P2 t{d[0] / l, d[1] / l}, nn = Left(t);
						// (u up the boards, 0 to 0.875; v along the fence, a picture's worth every 1.5 m)
						Writer::Quad(m, {P3{p0[0], p0[1], zp0}, P3{p0[0], p0[1], tp0}, P3{p1[0], p1[1], tp1}, P3{p1[0], p1[1], zp1}}, {nn[0], nn[1], 0}, t,
						    {Uv{0, s0 / 1.5f}, Uv{0.875f, s0 / 1.5f}, Uv{0.875f, s1 / 1.5f}, Uv{0, s1 / 1.5f}});
					};
					boards(out.boards, a0, a1, s[k], s[k + 1], za, zb, ta, tb);
					boards(out.boards, b1, b0, s[k + 1], s[k], zb, za, tb, ta);
					// (the top's edge)
					Writer::Quad(out.boards, {P3{a0[0], a0[1], ta}, P3{b0[0], b0[1], ta}, P3{b1[0], b1[1], tb}, P3{a1[0], a1[1], tb}}, {0, 0, 1}, dirOf(k),
					    {Uv{0.9f, s[k] / 1.5f}, Uv{0.95f, s[k] / 1.5f}, Uv{0.95f, s[k + 1] / 1.5f}, Uv{0.9f, s[k + 1] / 1.5f}});
					w.CollideFace(q[k], q[k + 1], za, zb, ta, tb, kWood, true);
					++stats.pieces;
				}
				posts([&](size_t k, const P2& d) { Writer::Box(out.boards, q[k], d, 0.045f, 0.06f, z[k] - 0.1f, z[k] + h + 0.05f, 0.5f, true); });
				++stats.fences;
				break;
			}
			case Kind::Railing:
			{
				// posts and two rails (at a third and at the top)
				for (size_t k = 0; k + 1 < q.size(); ++k)
				{
					if (!built[k])
						continue;
					const P2 d = dirOf(k), m{(q[k][0] + q[k + 1][0]) / 2, (q[k][1] + q[k + 1][1]) / 2};
					const float l = s[k + 1] - s[k], zm = (z[k] + z[k + 1]) / 2;
					for (const float at : {h * 0.35f, h - 0.03f})
						Writer::Box(out.steel, m, d, l / 2, 0.025f, zm + at - 0.03f, zm + at + 0.03f, 1.0f, true);
					w.CollideFace(q[k], q[k + 1], z[k] + 0.1f, z[k + 1] + 0.1f, z[k] + h, z[k + 1] + h, kRailing, false);
					++stats.pieces;
				}
				posts([&](size_t k, const P2& d) { Writer::Box(out.steel, q[k], d, 0.035f, 0.035f, z[k] - 0.1f, z[k] + h + 0.03f, 1.0f, true); });
				++stats.fences;
				break;
			}
			case Kind::Bars:
			{
				// the picture's panels (a panel every 1.22 m, from 8 cm over the ground to the spear tips), both faces, the
				// vertex colours the chain link's; square posts 6 cm across of the picture's top rail - black throughout
				const Colour foot{234, 234, 34, 255}, top{234, 234, 255, 255};
				roads::Mesh& pickets = b.material == "aluminium" ? out.ironWhite : out.iron; // (white aluminium, or black iron)
				for (size_t k = 0; k + 1 < q.size(); ++k)
				{
					if (!built[k])
						continue;
					const float za = z[k] + 0.08f, zb = z[k + 1] + 0.08f, ta = z[k] + h, tb = z[k + 1] + h, perMetre = 1 / (h - 0.08f);
					Writer::Face(pickets, q[k], q[k + 1], za, zb, ta, tb, s[k] / 1.22f, s[k + 1] / 1.22f, 1, perMetre, foot, top);
					Writer::Face(pickets, q[k + 1], q[k], zb, za, tb, ta, s[k + 1] / 1.22f, s[k] / 1.22f, 1, perMetre, foot, top);
					w.CollideFace(q[k], q[k + 1], za, zb, ta, tb, kRailing, false);
					++stats.pieces;
				}
				posts([&](size_t k, const P2& d) {
					const P2 n = Left(d);
					constexpr float half = 0.03f, rail = 0.105f; // (the top rail's middle in the picture)
					const std::array<P2, 4> c{P2{q[k][0] + (d[0] + n[0]) * half, q[k][1] + (d[1] + n[1]) * half},
					    P2{q[k][0] + (d[0] - n[0]) * half, q[k][1] + (d[1] - n[1]) * half}, P2{q[k][0] - (d[0] + n[0]) * half, q[k][1] - (d[1] + n[1]) * half},
					    P2{q[k][0] - (d[0] - n[0]) * half, q[k][1] - (d[1] - n[1]) * half}}; // (clockwise from above: outward is each side's left)
					for (size_t e = 0; e < 4; ++e)
						Writer::Face(pickets, c[e], c[(e + 1) % 4], z[k] - 0.1f, z[k] - 0.1f, z[k] + h + 0.03f, z[k] + h + 0.03f, 0.5f, 0.5f, rail, 0, foot, top);
				});
				++stats.fences;
				break;
			}
			case Kind::Wall:
			case Kind::Retaining:
			{
				// a wall in pieces: both faces, its top, its ends (painted block, the picture 2.6 m wide and 2.4 high; the
				// retaining walls' concrete as the roads' - a picture every 6 m)
				const bool block = kind == Kind::Wall;
				roads::Mesh& m = block ? out.block : out.concrete;
				const Colour c = block ? Colour{255, 255, 255, 255} : Colour{255, 12, 255, 255}; // (the roads' shader's: as their own vertices)
				const float half = block ? 0.1f : 0.15f, uScale = block ? 1 / 2.6f : 1 / 6.0f, vScale = block ? 1 / 2.4f : 1 / 6.0f;
				for (size_t k = 0; k + 1 < q.size(); ++k)
				{
					if (!built[k])
						continue;
					const P2 d = dirOf(k), n = Left(d);
					const P2 a0{q[k][0] + n[0] * half, q[k][1] + n[1] * half}, a1{q[k + 1][0] + n[0] * half, q[k + 1][1] + n[1] * half};
					const P2 b0{q[k][0] - n[0] * half, q[k][1] - n[1] * half}, b1{q[k + 1][0] - n[0] * half, q[k + 1][1] - n[1] * half};
					const float za = z[k] - 0.1f, zb = z[k + 1] - 0.1f, ta = z[k] + h, tb = z[k + 1] + h;
					Writer::Face(m, a0, a1, za, zb, ta, tb, s[k] * uScale, s[k + 1] * uScale, 1, vScale, c, c);
					Writer::Face(m, b1, b0, zb, za, tb, ta, s[k + 1] * uScale, s[k] * uScale, 1, vScale, c, c);
					Writer::Quad(m, {P3{a0[0], a0[1], ta}, P3{b0[0], b0[1], ta}, P3{b1[0], b1[1], tb}, P3{a1[0], a1[1], tb}}, {0, 0, 1}, d,
					    {Uv{s[k] * uScale, 0}, Uv{s[k] * uScale, 2 * half * vScale}, Uv{s[k + 1] * uScale, 2 * half * vScale}, Uv{s[k + 1] * uScale, 0}},
					    {c, c, c, c});
					// (an end where the wall stops)
					if (k == 0 || !built[k - 1])
						Writer::Face(m, b0, a0, za, za, ta, ta, 0, 2 * half * uScale, 1, vScale, c, c);
					if (k + 2 >= q.size() || !built[k + 1])
						Writer::Face(m, a1, b1, zb, zb, tb, tb, 0, 2 * half * uScale, 1, vScale, c, c);
					w.CollideFace(a0, a1, za, zb, ta, tb, kConcrete, false);
					w.CollideFace(b1, b0, zb, za, tb, ta, kConcrete, false);
					++stats.pieces;
				}
				++stats.walls;
				break;
			}
			case Kind::Hedge:
			{
				// the game's clipped bush (prop_bush_neat_04) a little apart along the line, its long side along it, 1.2 m
				// wide, as high as the hedge
				for (size_t k = 0; k + 1 < q.size(); ++k)
				{
					if (!built[k])
						continue;
					const P2 d = dirOf(k), m{(q[k][0] + q[k + 1][0]) / 2, (q[k][1] + q[k + 1][1]) / 2};
					props::Prop p;
					p.archetype = hedgeBush;
					p.position = {m[0], m[1], ground(m[0], m[1]) - 0.1f};
					p.heading = std::atan2(-d[0], d[1]) + ((seeds::PlaceAt(seeds::Between(q[k], q[k + 1]), 2, 22) & 1) ? 3.14159265f : 0.0f);
					p.lodDist = 150;
					p.scale = 0.6f;
					p.scaleZ = h / 1.5f;
					props.push_back(p);
					++stats.pieces;
				}
				++stats.hedges;
				break;
			}
			case Kind::GuardRail:
			{
				// the beam on the road's side of the posts (the side a road is on within 4 m; the left when none), 31 cm
				// deep, its top 76 cm up; the posts 15 x 10 cm
				const size_t mid = q.size() / 2 - 1;
				const P2 dm = dirOf(mid), nm = Left(dm), cm{(q[mid][0] + q[mid + 1][0]) / 2, (q[mid][1] + q[mid + 1][1]) / 2};
				const float side = onRoad(cm[0] - nm[0] * 4, cm[1] - nm[1] * 4) && !onRoad(cm[0] + nm[0] * 4, cm[1] + nm[1] * 4) ? -1.0f : 1.0f;
				for (size_t k = 0; k + 1 < q.size(); ++k)
				{
					if (!built[k])
						continue;
					const P2 n = Left(dirOf(k));
					const float off = 0.12f * side;
					const P2 a0{q[k][0] + n[0] * off, q[k][1] + n[1] * off}, a1{q[k + 1][0] + n[0] * off, q[k + 1][1] + n[1] * off};
					const float ta = z[k] + h, tb = z[k + 1] + h, ba = ta - 0.31f, bb = tb - 0.31f;
					// (the picture's upper half: the beam's face, a picture every 3.81 m)
					if (side > 0)
						Writer::Face(out.rail, a0, a1, ba, bb, ta, tb, s[k] / 3.81f, s[k + 1] / 3.81f, 0.5f, 0.5f / 0.31f);
					else
						Writer::Face(out.rail, a1, a0, bb, ba, tb, ta, s[k + 1] / 3.81f, s[k] / 3.81f, 0.5f, 0.5f / 0.31f);
					// (its back, plain)
					if (side > 0)
						Writer::Face(out.rail, a1, a0, bb, ba, tb, ta, s[k + 1] / 3.81f, s[k] / 3.81f, 0.5f, 0.5f / 0.31f);
					else
						Writer::Face(out.rail, a0, a1, ba, bb, ta, tb, s[k] / 3.81f, s[k + 1] / 3.81f, 0.5f, 0.5f / 0.31f);
					w.CollideFace(a0, a1, ba, bb, ta, tb, kRailing, true);
					++stats.pieces;
				}
				posts([&](size_t k, const P2& d) { Writer::Box(out.steel, q[k], d, 0.05f, 0.075f, z[k] - 0.1f, z[k] + h - 0.02f, 1.0f, true); });
				++stats.rails;
				break;
			}
			}
			for (size_t k = 0; k + 1 < q.size(); ++k)
				if (built[k])
					stats.metres += s[k + 1] - s[k];
		}
	}

	texture::Image IronPicture(bool white)
	{
		// 512 x 512 for 1.22 m by the fence's height: pickets 10 px wide every 51.2 px, spear tips over the top 28 rows, the
		// top rail rows 44-63 - solid across, the posts' -, the bottom rail rows 430-447, the pickets on to row 500; black
		// paint a little lighter down each bar's middle (a round look in the light)
		texture::Image img(512, 512, {18, 18, 20, 0});
		const auto paint = [&](int x, int y, float shade) {
			if (x < 0 || x >= 512 || y < 0 || y >= 512)
				return;
			const uint8_t c = static_cast<uint8_t>(std::clamp(white ? 196.0f + 50.0f * shade : 8.0f + 18.0f * shade, 0.0f, 255.0f));
			img.At(x, y) = {c, c, static_cast<uint8_t>(std::min(255, c + 2)), 255};
		};
		for (int p = 0; p < 12; ++p)
		{
			const float mid = (p + 0.5f) * (512.0f / 12);
			for (int y = 0; y < 500; ++y)
			{
				// (the tip: widening to 18 px at row 16, back to the bar's 10 px at row 28)
				const float half = y < 16 ? 9.0f * y / 16 : y < 28 ? 9.0f - 4.0f * (y - 16) / 12 : 5.0f;
				for (int x = static_cast<int>(std::floor(mid - half)); x <= static_cast<int>(std::ceil(mid + half)); ++x)
				{
					const float off = std::fabs(x + 0.5f - mid) / std::max(half, 1.0f);
					if (off <= 1)
						paint(x, y, 1 - off * off);
				}
			}
		}
		for (const auto [y0, y1] : {std::pair{44, 64}, std::pair{430, 448}})
			for (int y = y0; y < y1; ++y)
			{
				const float off = std::fabs(y + 0.5f - (y0 + y1) / 2.0f) / ((y1 - y0) / 2.0f);
				for (int x = 0; x < 512; ++x)
					paint(x, y, 0.8f * (1 - off * off));
			}
		return img;
	}

	std::vector<world::Barrier> Enclosures(const std::vector<world::Area>& areas, const std::vector<world::Barrier>& mapped,
	    const std::function<bool(float, float)>& byStreet, const std::function<bool(float, float)>& onPath)
	{
		// the map's barriers by 8 m cells (a fence along a cemetery's edge is often mapped as its own line)
		using Key = int64_t;
		const auto keyOf = [](float x, float y) {
			return static_cast<Key>(std::floor(x / 8)) << 32 | static_cast<uint32_t>(static_cast<int32_t>(std::floor(y / 8)));
		};
		std::unordered_map<Key, std::vector<std::array<P2, 2>>> near;
		for (const world::Barrier& b : mapped)
			for (size_t k = 0; k + 1 < b.points.size(); ++k)
			{
				const P2 &a = b.points[k], &c = b.points[k + 1];
				for (float y = std::floor((std::min(a[1], c[1]) - 4) / 8) * 8; y <= std::max(a[1], c[1]) + 4; y += 8)
					for (float x = std::floor((std::min(a[0], c[0]) - 4) / 8) * 8; x <= std::max(a[0], c[0]) + 4; x += 8)
						near[keyOf(x + 1, y + 1)].push_back({a, c});
			}
		const auto mappedNear = [&](const P2& p) {
			const auto it = near.find(keyOf(p[0], p[1]));
			if (it == near.end())
				return false;
			for (const auto& [a, c] : it->second)
			{
				const float dx = c[0] - a[0], dy = c[1] - a[1], l = dx * dx + dy * dy;
				const float t = l > 0 ? std::clamp(((p[0] - a[0]) * dx + (p[1] - a[1]) * dy) / l, 0.0f, 1.0f) : 0.0f;
				if (std::hypot(p[0] - a[0] - dx * t, p[1] - a[1] - dy * t) < 4)
					return true;
			}
			return false;
		};
		std::vector<world::Barrier> out;
		for (const world::Area& a : areas)
		{
			// (a cemetery's wrought iron; a construction site's chain link - the street pictures' building sites are fenced)
			const bool site = a.kind == "construction";
			if ((a.kind != "cemetery" || props::MemorialPark(a.name)) && !site)
				continue;
			for (const auto& ring : a.outer)
			{
				// (the outline in pieces of up to 4 m; a piece fenced when its middle is by a street, off the paths and
				// clear of the map's barriers; the fenced pieces one after another one fence - round the ring's start too)
				std::vector<P2> q;
				for (size_t k = 0; k < ring.size(); ++k)
				{
					const P2 &p = ring[k], &c = ring[(k + 1) % ring.size()];
					const int n = std::max(1, static_cast<int>(std::ceil(std::hypot(c[0] - p[0], c[1] - p[1]) / 4)));
					for (int j = 0; j < n; ++j)
						q.push_back({p[0] + (c[0] - p[0]) * j / n, p[1] + (c[1] - p[1]) * j / n});
				}
				const size_t count = q.size();
				std::vector<bool> fenced(count);
				for (size_t k = 0; k < count; ++k)
				{
					const P2 &p = q[k], &c = q[(k + 1) % count], m{(p[0] + c[0]) / 2, (p[1] + c[1]) / 2};
					fenced[k] = byStreet(m[0], m[1]) && !onPath(m[0], m[1]) && !mappedNear(m);
				}
				size_t start = 0;
				while (start < count && fenced[start])
					++start;
				if (start == count) // (all of it: round from the first point)
					start = 0;
				world::Barrier b;
				const auto flush = [&] {
					if (b.points.size() >= 2)
					{
						b.kind = "fence";
						b.material = site ? "chain_link" : "bars";
						b.height = 1.8f;
						out.push_back(b);
					}
					b.points.clear();
				};
				for (size_t n = 0; n < count; ++n)
				{
					const size_t k = (start + n) % count;
					if (!fenced[k])
					{
						flush();
						continue;
					}
					if (b.points.empty())
						b.points.push_back(q[k]);
					b.points.push_back(q[(k + 1) % count]);
				}
				flush();
			}
		}
		return out;
	}
}
