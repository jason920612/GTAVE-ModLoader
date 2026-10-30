#include "powerlines.hpp"

#include <algorithm>
#include <cmath>
#include <cstdint>

#include "convert/resource.hpp"
#include "seeds.hpp"

namespace worldgen::powerlines
{
	namespace
	{
		using P2 = std::array<float, 2>;
		using P3 = std::array<float, 3>;

		uint32_t Mix(uint32_t h)
		{
			h ^= h >> 16;
			h *= 0x7feb352d;
			h ^= h >> 15;
			h *= 0x846ca68b;
			return h ^ (h >> 16);
		}

		P2 Unit(P2 v)
		{
			const float l = std::hypot(v[0], v[1]);
			return l > 1e-6f ? P2{v[0] / l, v[1] / l} : P2{1, 0};
		}

		// the poles' models, their insulators' tops over the base (the transformer's crossarm 0.3 m higher)
		constexpr const char* kPlain = "prop_telegraph_01b";
		constexpr const char* kTransformer = "prop_telegraph_01a";
		constexpr float kTransformerRise = 0.3f;

		// the wires: x along the crossarm (m), height over the base, half width; three on the crossarm's insulators (the
		// models' at -1.05, -0.3, 0.3 and 1.05 m), the communications cable on the pole's side, thicker and sagging more (4 and
		// 6 cm across: at 6 and 9 they had been "one very thick, pure-black cable" to round 27's reviewers)
		struct Wire
		{
			float x, z, halfWidth, sag; // (sag: of the span's length)
			bool onArm;
		};
		constexpr Wire kWires[] = {{-1.05f, 14.2f, 0.02f, 0.022f, true}, {0.3f, 14.2f, 0.02f, 0.022f, true}, {1.05f, 14.2f, 0.02f, 0.022f, true},
		    {0.2f, 8.6f, 0.03f, 0.03f, false}};

		constexpr float kSpacing = 40; // (m between poles)
		constexpr float kLongest = 72; // (the longest span, m: a pole left out past that breaks the line)

		// the streets and roads that carry power lines: not the motorways and their ramps, bridges, driveways or paths
		bool Carries(const roads::Edge& e)
		{
			return !e.bridge && !roads::Minor(e) && !e.kind.starts_with("motorway") && !e.kind.ends_with("_link") && e.points.size() >= 2;
		}

		float Length(const roads::Edge& e)
		{
			float l = 0;
			for (size_t i = 0; i + 1 < e.points.size(); ++i)
				l += std::hypot(e.points[i + 1][0] - e.points[i][0], e.points[i + 1][1] - e.points[i][1]);
			return l;
		}

		// the point at distance s along the road's line and its direction
		bool At(const roads::Edge& e, float s, P2& p, P2& d, seeds::G* g = nullptr)
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
					// (and in the real world's plane in doubles: the same in every land - p, in floats of the land's
					// coordinates, not quite)
					if (g)
						*g = seeds::Between(a, b, t);
					return true;
				}
				run += l;
			}
			return false;
		}

		// how far along the road's line the point nearest q is
		float Along(const roads::Edge& e, const P2& q)
		{
			float run = 0, best = 1e30f, at = 0;
			for (size_t i = 0; i + 1 < e.points.size(); ++i)
			{
				const P2& a = e.points[i];
				const P2& b = e.points[i + 1];
				const float l = std::hypot(b[0] - a[0], b[1] - a[1]);
				if (l <= 0)
					continue;
				const float t = std::clamp(((q[0] - a[0]) * (b[0] - a[0]) + (q[1] - a[1]) * (b[1] - a[1])) / (l * l), 0.0f, 1.0f);
				const float d = std::hypot(a[0] + (b[0] - a[0]) * t - q[0], a[1] + (b[1] - a[1]) * t - q[1]);
				if (d < best)
					best = d, at = run + t * l;
				run += l;
			}
			return at;
		}

		struct Pole
		{
			P2 p{};
			seeds::G g; // (its place in the real world's plane: what its choices are keyed to)
			float z = 0;
			P2 road{}; // the road's direction by it (a pole with no wires: its crossarm across the road)
			P2 toRoad{}; // from it to the road
			bool transformer = false;
			bool lamp = false; // (a street light on it)
			std::vector<int> spans; // the poles it is wired to
			float heading = 0;
		};
	}

	namespace
	{
		// A cobra-head street light on a pole standing at `foot`: its arm a tube 4 cm thick from the pole's side 9.2 m up out
		// 2.3 m towards `out`, rising 0.35 m; the luminaire at its end a tapered box 0.8 m long, 0.34 m wide at its back and 0.22
		// at its front, 0.17 m deep, its flat lens underneath.
		void Lamp(roads::Mesh& m, const P3& foot, const P2& out)
		{
			const P3 side{-out[1], out[0], 0};
			const auto at = [&](float along, float across, float up) {
				return P3{foot[0] + out[0] * along + side[0] * across, foot[1] + out[1] * along + side[1] * across, foot[2] + up};
			};
			const auto vertex = [&](const P3& p, const P3& n, float u, float v) {
				ydr::Vertex x;
				x.position = p;
				x.normal = n;
				x.tangent = {out[0], out[1], 0, 1};
				x.colour0 = {255, 255, 255, 255};
				x.uv[0] = {u, v};
				m.vertices.push_back(x);
				return static_cast<uint32_t>(m.vertices.size() - 1);
			};
			// the arm: six sides round its line
			constexpr int kSides = 6;
			constexpr float kR = 0.04f, kPi = 3.14159265f;
			const P3 a = at(0.13f, 0, 9.2f), b = at(2.3f, 0, 9.55f);
			const float len = std::hypot(b[0] - a[0], b[1] - a[1], b[2] - a[2]);
			const P3 dir{(b[0] - a[0]) / len, (b[1] - a[1]) / len, (b[2] - a[2]) / len};
			const P3 upish{-dir[0] * dir[2], -dir[1] * dir[2], 1 - dir[2] * dir[2]};
			const float ul = std::hypot(upish[0], upish[1], upish[2]);
			const P3 up{upish[0] / ul, upish[1] / ul, upish[2] / ul};
			const uint32_t first = static_cast<uint32_t>(m.vertices.size());
			for (int end = 0; end < 2; ++end)
				for (int k = 0; k <= kSides; ++k)
				{
					const float t = 2 * kPi * k / kSides, c = std::cos(t), s = std::sin(t);
					const P3 n{side[0] * c + up[0] * s, side[1] * c + up[1] * s, side[2] * c + up[2] * s};
					const P3& o = end ? b : a;
					vertex({o[0] + n[0] * kR, o[1] + n[1] * kR, o[2] + n[2] * kR}, n, static_cast<float>(k) / kSides, end ? len : 0.0f);
				}
			for (int k = 0; k < kSides; ++k)
			{
				const uint32_t p0 = first + static_cast<uint32_t>(k), p1 = p0 + 1, q0 = p0 + kSides + 1, q1 = q0 + 1;
				m.indices.insert(m.indices.end(), {p0, q0, q1, p0, q1, p1});
			}
			// the luminaire: its eight corners (back to front, left and right, bottom and top)
			const float z0 = 9.44f, z1 = 9.61f;
			const P3 c[8] = {at(2.15f, -0.17f, z0), at(2.15f, 0.17f, z0), at(2.95f, 0.11f, z0 + 0.03f), at(2.95f, -0.11f, z0 + 0.03f),
			    at(2.15f, -0.17f, z1), at(2.15f, 0.17f, z1), at(2.95f, 0.11f, z1 - 0.04f), at(2.95f, -0.11f, z1 - 0.04f)};
			const auto face = [&](int i0, int i1, int i2, int i3) {
				const P3 &p = c[i0], &q = c[i1], &r = c[i2];
				const P3 u{q[0] - p[0], q[1] - p[1], q[2] - p[2]}, v{r[0] - p[0], r[1] - p[1], r[2] - p[2]};
				P3 n{u[1] * v[2] - u[2] * v[1], u[2] * v[0] - u[0] * v[2], u[0] * v[1] - u[1] * v[0]};
				const float l = std::max(1e-6f, std::hypot(n[0], n[1], n[2]));
				n = {n[0] / l, n[1] / l, n[2] / l};
				const uint32_t j0 = vertex(c[i0], n, 0, 0), j1 = vertex(c[i1], n, 1, 0), j2 = vertex(c[i2], n, 1, 1), j3 = vertex(c[i3], n, 0, 1);
				m.indices.insert(m.indices.end(), {j0, j1, j2, j0, j2, j3});
			};
			// (each anticlockwise seen from outside)
			face(0, 3, 2, 1); // (the lens, underneath)
			face(4, 5, 6, 7); // (the top)
			face(0, 1, 5, 4); // (the back)
			face(3, 7, 6, 2); // (the front)
			face(0, 4, 7, 3); // (the sides)
			face(1, 2, 6, 5);
		}
	}

	void Place(const roads::Network& n, const std::function<bool(float, float)>& clear, const std::function<float(float, float)>& ground,
	    const std::function<bool(float, float)>& buried, const std::array<float, 4>& box, std::vector<props::Prop>& poles, roads::Mesh& wires,
	    Stats& stats, const std::function<float(const roads::Edge&, std::vector<std::array<float, 2>>&)>& measured, roads::Mesh* lamps)
	{
		std::vector<Pole> all;
		std::vector<std::vector<int>> edgePoles(n.edges.size()); // (each road's, from its a to its b)
		const auto wire = [&](int i, int j) {
			if (i == j || std::find(all[i].spans.begin(), all[i].spans.end(), j) != all[i].spans.end())
				return;
			if (std::hypot(all[i].p[0] - all[j].p[0], all[i].p[1] - all[j].p[1]) > kLongest)
				return;
			all[i].spans.push_back(j);
			all[j].spans.push_back(i);
		};
		for (size_t ei = 0; ei < n.edges.size(); ++ei)
		{
			const roads::Edge& e = n.edges[ei];
			if (!Carries(e) || e.a == e.b)
				continue;
			if (buried)
			{
				size_t under = 0, samples = 0;
				for (size_t k = 0; k < e.points.size(); k += std::max<size_t>(1, e.points.size() / 8))
					under += buried(e.points[k][0], e.points[k][1]), ++samples;
				if (under * 3 >= samples)
					continue;
			}
			const float total = Length(e);
			// (clear of a street junction's corners, as the street lights are; half a span from where the road just goes on,
			// the next road's poles the other half away; a little from a dead end)
			const auto margin = [&](int v) {
				const size_t degree = n.vertices[static_cast<size_t>(v)].edges.size();
				return degree >= 3 ? roads::CornerReach(n, v) + 3 : degree == 2 ? kSpacing / 2 : 6.0f;
			};
			const float s0 = margin(e.a), s1 = total - margin(e.b);
			std::vector<float> at;
			if (s1 - s0 >= 20)
			{
				const int spans = std::max(1, static_cast<int>(std::lround((s1 - s0) / kSpacing)));
				// (the ones between the ends up to 18 % of a span off even, by the road - the same in every land that has it: an
				// even 40 m had read as "identical clones in a perfectly even straight line")
				for (int k = 0; k <= spans; ++k)
				{
					const float jitter = k == 0 || k == spans ? 0.0f
					                                          : (static_cast<float>(seeds::Mix(static_cast<uint32_t>(e.id) * 2654435761u + static_cast<uint32_t>(k)) >> 8) / 16777216.0f -
					                                                0.5f) * 0.36f;
					at.push_back(s0 + (s1 - s0) * (static_cast<float>(k) + jitter) / static_cast<float>(spans));
				}
			}
			else if (total >= 20)
				at.push_back(std::clamp((s0 + s1) / 2, 4.0f, total - 4));
			// the side: a one-way road's outer one (the right of the way it goes); a two-way road's north-eastern one, by the
			// road's general direction, so that a road going on in another way of the map keeps to its side
			const P2 chord = Unit({e.points.back()[0] - e.points.front()[0], e.points.back()[1] - e.points.front()[1]});
			std::vector<P2> scanned;
			const float measuredSide = measured ? measured(e, scanned) : 0.0f;
			const float side = measuredSide != 0                       ? measuredSide
			                 : e.forward > 0 && e.backward == 0   ? -1.0f
			                 : e.forward == 0 && e.backward > 0 ? 1.0f
			                 : -chord[1] * 0.6f + chord[0] * 0.8f >= 0 ? 1.0f
			                                                           : -1.0f;
			// (behind the sidewalk, or in the verge 2 m off the asphalt)
			const float off = e.HalfWidth() + (e.sidewalks ? e.Walk() + 0.9f : 2.0f);
			// (the streets without street lights of their own - props.cpp - have theirs on every other pole)
			const bool lit = e.kind == "residential" || e.kind == "unclassified" || e.kind == "living_street" || e.kind == "road";
			// the poles the scan shows, where it shows two or more on the line's side: each where it stands, one more evenly
			// between two farther apart than 60 m
			if (measuredSide != 0 && scanned.size() >= 2)
			{
				std::vector<std::pair<float, P2>> found;
				for (const P2& q : scanned)
					found.push_back({Along(e, q), q});
				std::sort(found.begin(), found.end(), [](const auto& x, const auto& y) { return x.first < y.first; });
				std::vector<std::pair<float, P2>> kept;
				for (const auto& f : found)
					if (kept.empty() || f.first - kept.back().first >= 8)
						kept.push_back(f);
				const auto add = [&](const P2& q, float s, uint32_t seed) {
					P2 p, d;
					if (!At(e, std::clamp(s, 0.0f, total), p, d))
						d = Unit({e.points.back()[0] - e.points.front()[0], e.points.back()[1] - e.points.front()[1]});
					Pole pole;
					pole.p = q;
					pole.g = {seeds::GX(q[0]), seeds::GY(q[1])};
					pole.z = ground(q[0], q[1]) - 0.2f;
					pole.road = d;
					pole.toRoad = Unit({p[0] - q[0], p[1] - q[1]});
					pole.transformer = (seed >> 12) % 7 == 0;
					pole.lamp = lit && edgePoles[ei].size() % 2 == 0;
					edgePoles[ei].push_back(static_cast<int>(all.size()));
					all.push_back(std::move(pole));
				};
				for (size_t k = 0; k < kept.size(); ++k)
				{
					const uint32_t seed = seeds::Place(kept[k].second[0], kept[k].second[1], 2, 17);
					if (k > 0)
					{
						const float gap = kept[k].first - kept[k - 1].first;
						const int extra = gap > 60 ? static_cast<int>(std::ceil(gap / 45)) - 1 : 0;
						for (int x = 1; x <= extra; ++x)
						{
							const float s = kept[k - 1].first + gap * static_cast<float>(x) / static_cast<float>(extra + 1);
							P2 p, d;
							if (!At(e, s, p, d))
								continue;
							const P2 q{p[0] - d[1] * side * off, p[1] + d[0] * side * off};
							if (clear(q[0], q[1]))
								add(q, s, Mix(seed + static_cast<uint32_t>(x)));
						}
					}
					if (clear(kept[k].second[0], kept[k].second[1]))
						add(kept[k].second, kept[k].first, seed);
				}
				at.clear();
			}
			for (size_t k = 0; k < at.size(); ++k)
			{
				P2 here, along;
				seeds::G place;
				const uint32_t seed =
				    At(e, std::clamp(at[k], 0.0f, total), here, along, &place) ? seeds::PlaceAt(place, 2, 18) : seeds::Mix(static_cast<uint32_t>(k));
				float s = at[k];
				if (k > 0 && k + 1 < at.size())
					s += static_cast<float>(static_cast<int>(seed % 51) - 25) / 10; // (not quite even)
				// (where something stands in the way, a little along the road)
				for (const float shift : {0.0f, 3.0f, -3.0f, 6.0f, -6.0f, 9.0f, -9.0f})
				{
					P2 p, d;
					seeds::G g;
					if (!At(e, std::clamp(s + shift, 0.0f, total), p, d, &g))
						break;
					const P2 q{p[0] - d[1] * side * off, p[1] + d[0] * side * off};
					if (!clear(q[0], q[1]))
						continue;
					Pole pole;
					pole.p = q;
					pole.g = {g.x - static_cast<double>(d[1]) * side * off, g.y + static_cast<double>(d[0]) * side * off};
					pole.z = ground(q[0], q[1]) - 0.2f;
					pole.road = d;
					pole.toRoad = {d[1] * side, -d[0] * side};
					pole.transformer = (seed >> 12) % 7 == 0;
					pole.lamp = lit && edgePoles[ei].size() % 2 == 0;
					edgePoles[ei].push_back(static_cast<int>(all.size()));
					all.push_back(std::move(pole));
					break;
				}
			}
			for (size_t k = 1; k < edgePoles[ei].size(); ++k)
				wire(edgePoles[ei][k - 1], edgePoles[ei][k]);
		}
		// across the junctions: a road's line on into the road going straight on (where the road only bends, on into the
		// next whatever the bend), the straightest first
		for (size_t vi = 0; vi < n.vertices.size(); ++vi)
		{
			const auto& v = n.vertices[vi];
			if (v.edges.size() < 2)
				continue;
			struct End
			{
				int pole;
				P2 out; // (the road's direction away from the junction)
			};
			std::vector<End> ends;
			for (const int ei : v.edges)
			{
				const roads::Edge& e = n.edges[static_cast<size_t>(ei)];
				const auto& list = edgePoles[static_cast<size_t>(ei)];
				if (list.empty() || e.a == e.b)
					continue;
				if (e.a == static_cast<int>(vi))
					ends.push_back({list.front(), Unit({e.points[1][0] - e.points[0][0], e.points[1][1] - e.points[0][1]})});
				else
				{
					const size_t m = e.points.size();
					ends.push_back({list.back(), Unit({e.points[m - 2][0] - e.points[m - 1][0], e.points[m - 2][1] - e.points[m - 1][1]})});
				}
			}
			const float straight = v.edges.size() == 2 ? 0.5f : -0.8f;
			std::vector<std::pair<float, std::pair<size_t, size_t>>> pairs;
			for (size_t i = 0; i < ends.size(); ++i)
				for (size_t j = i + 1; j < ends.size(); ++j)
				{
					const float dot = ends[i].out[0] * ends[j].out[0] + ends[i].out[1] * ends[j].out[1];
					if (dot < straight)
						pairs.push_back({dot, {i, j}});
				}
			std::sort(pairs.begin(), pairs.end());
			std::vector<bool> used(ends.size(), false);
			for (const auto& [dot, ij] : pairs)
				if (!used[ij.first] && !used[ij.second])
				{
					used[ij.first] = used[ij.second] = true;
					wire(ends[ij.first].pole, ends[ij.second].pole);
				}
		}
		// each pole's crossarm across its line: across the line through its two neighbours, or along the one wire it has
		for (Pole& pole : all)
		{
			P2 dir = pole.road;
			if (pole.spans.size() >= 2)
			{
				// (the two in the order of their places in the real world, not of the wires' making - each land numbers its
				// roads its own way, and the crossarm came round the other way)
				const Pole *pa = &all[static_cast<size_t>(pole.spans[0])], *pb = &all[static_cast<size_t>(pole.spans[1])];
				if (std::make_pair(pb->g.x, pb->g.y) < std::make_pair(pa->g.x, pa->g.y))
					std::swap(pa, pb);
				const P2& a = pa->p;
				const P2& b = pb->p;
				const P2 da = Unit({pole.p[0] - a[0], pole.p[1] - a[1]}), db = Unit({b[0] - pole.p[0], b[1] - pole.p[1]});
				dir = std::hypot(da[0] + db[0], da[1] + db[1]) > 0.2f ? Unit({da[0] + db[0], da[1] + db[1]}) : da;
			}
			else if (pole.spans.size() == 1)
			{
				const P2& a = all[static_cast<size_t>(pole.spans[0])].p;
				dir = Unit({a[0] - pole.p[0], a[1] - pole.p[1]});
			}
			// (the model's x axis - its crossarm - to the right of the line)
			pole.heading = std::atan2(-dir[0], dir[1]);
		}
		const auto inBox = [&](const P2& p) { return p[0] >= box[0] && p[1] >= box[1] && p[0] < box[2] && p[1] < box[3]; };
		const uint32_t plain = loader::convert::Joaat(kPlain), transformer = loader::convert::Joaat(kTransformer);
		for (const Pole& pole : all)
			if (inBox(pole.p))
			{
				poles.push_back({pole.transformer ? transformer : plain, {pole.p[0], pole.p[1], pole.z}, pole.heading, 350});
				++stats.poles;
				if (pole.lamp && lamps)
					Lamp(*lamps, {pole.p[0], pole.p[1], pole.z}, pole.toRoad);
			}
		// the wires of each span (both its poles here): each from its place on one pole to the same place on the other,
		// the crossarms' sides matched by the span's own right
		for (size_t i = 0; i < all.size(); ++i)
			for (const int jj : all[i].spans)
			{
				const size_t j = static_cast<size_t>(jj);
				if (j < i || !inBox(all[i].p) || !inBox(all[j].p))
					continue;
				const Pole &a = all[i], &b = all[j];
				const P2 d = Unit({b.p[0] - a.p[0], b.p[1] - a.p[1]});
				const P2 right{d[1], -d[0]};
				const P2 xa{std::cos(a.heading), std::sin(a.heading)}, xb{std::cos(b.heading), std::sin(b.heading)};
				const float sa = xa[0] * right[0] + xa[1] * right[1] >= 0 ? 1.0f : -1.0f;
				const float sb = xb[0] * right[0] + xb[1] * right[1] >= 0 ? 1.0f : -1.0f;
				for (size_t k = 0; k < std::size(kWires); ++k)
				{
					const Wire& w = kWires[k];
					const P3 from{a.p[0] + xa[0] * w.x * sa, a.p[1] + xa[1] * w.x * sa, a.z + w.z + (a.transformer && w.onArm ? kTransformerRise : 0)};
					const P3 to{b.p[0] + xb[0] * w.x * sb, b.p[1] + xb[1] * w.x * sb, b.z + w.z + (b.transformer && w.onArm ? kTransformerRise : 0)};
					const P3 span{to[0] - from[0], to[1] - from[1], to[2] - from[2]};
					const float length = std::sqrt(span[0] * span[0] + span[1] * span[1] + span[2] * span[2]);
					const float sag = w.sag * length;
					const int segments = std::clamp(static_cast<int>(std::lround(length / 7)), 4, 8);
					const uint32_t phase = seeds::PlaceAt(a.g, 2, static_cast<uint32_t>(k)) ^ seeds::PlaceAt(b.g, 2, 19);
					const uint32_t base = static_cast<uint32_t>(wires.vertices.size());
					for (int s = 0; s <= segments; ++s)
					{
						const float t = static_cast<float>(s) / static_cast<float>(segments);
						const float drop = 4 * sag * t * (1 - t);
						const P3 tangent{span[0], span[1], span[2] - 4 * sag * (1 - 2 * t)};
						const float tl = std::sqrt(tangent[0] * tangent[0] + tangent[1] * tangent[1] + tangent[2] * tangent[2]);
						for (const float sideOf : {-1.0f, 1.0f})
						{
							ydr::Vertex v;
							v.position = {from[0] + span[0] * t, from[1] + span[1] * t, from[2] + span[2] * t - drop};
							v.normal = {tangent[0] / tl, tangent[1] / tl, tangent[2] / tl};
							v.colour0 = {static_cast<uint8_t>(phase & 255), static_cast<uint8_t>((phase >> 8) & 255), static_cast<uint8_t>(std::lround(255 * t)), 255};
							v.uv[0] = {sideOf * w.halfWidth, drop};
							wires.vertices.push_back(v);
						}
					}
					for (int s = 0; s < segments; ++s)
					{
						const uint32_t a0 = base + 2 * static_cast<uint32_t>(s), a1 = a0 + 1, b0 = a0 + 2, b1 = a0 + 3;
						wires.indices.insert(wires.indices.end(), {a0, b0, a1, a1, b0, b1});
					}
					++stats.wires;
				}
				++stats.spans;
			}
	}
}
