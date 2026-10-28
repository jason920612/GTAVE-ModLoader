#include "structures.hpp"

#include <algorithm>
#include <functional>
#include <thread>
#include <atomic>
#include <cmath>
#include <limits>
#include <map>
#include <queue>
#include <tuple>

#include "polygon.hpp"
#include "raster.hpp"

namespace worldgen::structures
{
	namespace
	{
		double Orient(const P3& a, const P3& b, const P3& c)
		{
			return (b[0] - a[0]) * (c[1] - a[1]) - (b[1] - a[1]) * (c[0] - a[0]);
		}

		// d inside the circle through a, b, c (anticlockwise)
		bool InCircle(const P3& a, const P3& b, const P3& c, const P3& d)
		{
			const double adx = a[0] - d[0], ady = a[1] - d[1], bdx = b[0] - d[0], bdy = b[1] - d[1], cdx = c[0] - d[0], cdy = c[1] - d[1];
			const double ad = adx * adx + ady * ady, bd = bdx * bdx + bdy * bdy, cd = cdx * cdx + cdy * cdy;
			return adx * (bdy * cd - bd * cdy) - ady * (bdx * cd - bd * cdx) + ad * (bdx * cdy - bdy * cdx) > 1e-9;
		}

		// The roof: a triangulation in the plane (heights carried along), refined by inserting measured points.
		class Tin
		{
		public:
			struct Tri
			{
				std::array<int, 3> v{};
				std::array<int, 3> n{-1, -1, -1}; // the neighbour across edge k (v[k] -> v[k + 1]); -1: the outline
				std::vector<int> points;          // candidates inside
				int stamp = 0;
				bool alive = true;
			};
			std::vector<P3> verts;
			std::vector<Tri> tris;
			std::vector<P3> cands;

			bool Init(const std::vector<P2>& ring, const std::vector<double>& heights)
			{
				for (size_t i = 0; i < ring.size(); ++i)
					verts.push_back({ring[i][0], ring[i][1], heights[i]});
				const auto t = polygon::Triangulate(ring);
				if (t.empty())
					return false;
				std::map<std::pair<int, int>, std::pair<int, int>> edges;
				for (const auto& f : t)
				{
					Tri tri;
					tri.v = {static_cast<int>(f[0]), static_cast<int>(f[1]), static_cast<int>(f[2])};
					if (Orient(verts[tri.v[0]], verts[tri.v[1]], verts[tri.v[2]]) <= 0)
						continue;
					tris.push_back(tri);
				}
				for (int ti = 0; ti < static_cast<int>(tris.size()); ++ti)
					for (int k = 0; k < 3; ++k)
						edges[{tris[ti].v[k], tris[ti].v[(k + 1) % 3]}] = {ti, k};
				for (Tri& tri : tris)
					for (int k = 0; k < 3; ++k)
						if (const auto it = edges.find({tri.v[(k + 1) % 3], tri.v[k]}); it != edges.end())
							tri.n[k] = it->second.first;
				// Delaunay inside the outline (better triangles for the points to come)
				std::vector<std::pair<int, int>> todo;
				for (int ti = 0; ti < static_cast<int>(tris.size()); ++ti)
					for (int k = 0; k < 3; ++k)
						todo.push_back({ti, k});
				for (size_t guard = 0; !todo.empty() && guard < 20000; ++guard)
				{
					const auto [ti, k] = todo.back();
					todo.pop_back();
					if (!tris[ti].alive || tris[ti].n[k] < 0 || !NeedsFlip(ti, k))
						continue;
					const int u = tris[ti].n[k];
					if (!Flip(ti, k))
						continue;
					for (int kk = 0; kk < 3; ++kk)
						todo.push_back({ti, kk}), todo.push_back({u, kk});
				}
				return !tris.empty();
			}

			// candidates into the triangles that hold them
			void Distribute()
			{
				for (Tri& t : tris)
					t.points.clear();
				for (int c = 0; c < static_cast<int>(cands.size()); ++c)
				{
					int best = -1;
					double bestScore = -1e300;
					for (int ti = 0; ti < static_cast<int>(tris.size()); ++ti)
					{
						const double s = Score(ti, cands[c]);
						if (s > bestScore)
							bestScore = s, best = ti;
						if (s >= 0)
							break;
					}
					if (best >= 0 && bestScore > -0.05)
						tris[best].points.push_back(c);
				}
			}

			// the smallest of the point's barycentric distances to the triangle's edges (m; >= 0 inside)
			double Score(int ti, const P3& p) const
			{
				const Tri& t = tris[ti];
				double s = 1e300;
				for (int k = 0; k < 3; ++k)
				{
					const P3& a = verts[t.v[k]];
					const P3& b = verts[t.v[(k + 1) % 3]];
					const double len = std::hypot(b[0] - a[0], b[1] - a[1]);
					if (len < 1e-9)
						return -1e300;
					s = std::min(s, Orient(a, b, p) / len);
				}
				return s;
			}

			double Error(int ti, int c) const
			{
				const Tri& t = tris[ti];
				const P3 &a = verts[t.v[0]], &b = verts[t.v[1]], &cc = verts[t.v[2]];
				const P3& p = cands[c];
				const double d = Orient(a, b, cc);
				if (std::fabs(d) < 1e-12)
					return 0;
				const double w0 = Orient(b, cc, p) / d, w1 = Orient(cc, a, p) / d;
				return std::fabs(p[2] - (w0 * a[2] + w1 * b[2] + (1 - w0 - w1) * cc[2]));
			}

			// the candidate of a triangle farthest from it (-1: none); candidates on its corners are dropped
			int Worst(int ti, double& error)
			{
				Tri& t = tris[ti];
				int worst = -1;
				error = 0;
				std::erase_if(t.points, [&](int c) {
					for (const int v : t.v)
						if (std::hypot(cands[c][0] - verts[v][0], cands[c][1] - verts[v][1]) < 0.08)
							return true;
					return false;
				});
				for (const int c : t.points)
					if (const double e = Error(ti, c); e > error)
						error = e, worst = c;
				return worst;
			}

			void Insert(int ti, int c, std::vector<int>& changed)
			{
				const P3 p = cands[c];
				// on an edge (or as good as): split the edge
				for (int k = 0; k < 3; ++k)
				{
					const P3& a = verts[tris[ti].v[k]];
					const P3& b = verts[tris[ti].v[(k + 1) % 3]];
					const double len = std::hypot(b[0] - a[0], b[1] - a[1]);
					if (len > 1e-9 && std::fabs(Orient(a, b, p)) / len < 0.01)
						return SplitEdge(ti, k, p, changed);
				}
				const int q = static_cast<int>(verts.size());
				verts.push_back(p);
				const Tri old = tris[ti];
				const int a = old.v[0], b = old.v[1], cc = old.v[2];
				const int t0 = ti, t1 = static_cast<int>(tris.size()), t2 = t1 + 1;
				tris.resize(tris.size() + 2);
				tris[t0].v = {a, b, q}, tris[t0].n = {old.n[0], t1, t2};
				tris[t1].v = {b, cc, q}, tris[t1].n = {old.n[1], t2, t0};
				tris[t2].v = {cc, a, q}, tris[t2].n = {old.n[2], t0, t1};
				Repoint(old.n[1], ti, t1);
				Repoint(old.n[2], ti, t2);
				Redistribute(old.points, {t0, t1, t2});
				changed.insert(changed.end(), {t0, t1, t2});
				Legalize(t0, 0, changed);
				Legalize(t1, 0, changed);
				Legalize(t2, 0, changed);
			}

		private:
			void Repoint(int t, int from, int to)
			{
				if (t < 0)
					return;
				for (int& n : tris[t].n)
					if (n == from)
						n = to;
			}

			void Redistribute(const std::vector<int>& points, std::initializer_list<int> targets)
			{
				for (const int t : targets)
					tris[t].points.clear();
				for (const int c : points)
				{
					int best = -1;
					double bestScore = -1e300;
					for (const int t : targets)
						if (const double s = Score(t, cands[c]); s > bestScore)
							bestScore = s, best = t;
					if (best >= 0)
						tris[best].points.push_back(c);
				}
			}

			bool NeedsFlip(int ti, int k) const
			{
				const Tri& t = tris[ti];
				const int u = t.n[k];
				if (u < 0)
					return false;
				const int a = t.v[k], b = t.v[(k + 1) % 3], p = t.v[(k + 2) % 3];
				const Tri& o = tris[u];
				int ku = -1;
				for (int kk = 0; kk < 3; ++kk)
					if (o.v[kk] == b && o.v[(kk + 1) % 3] == a)
						ku = kk;
				if (ku < 0)
					return false;
				const int d = o.v[(ku + 2) % 3];
				return InCircle(verts[a], verts[b], verts[p], verts[d]);
			}

			// turns the edge k of ti and its neighbour's into the other diagonal of their quadrilateral: ti becomes
			// (a, d, p), the neighbour (d, b, p)
			bool Flip(int ti, int k)
			{
				const int u = tris[ti].n[k];
				const Tri t = tris[ti], o = tris[u];
				const int a = t.v[k], b = t.v[(k + 1) % 3], p = t.v[(k + 2) % 3];
				int ku = -1;
				for (int kk = 0; kk < 3; ++kk)
					if (o.v[kk] == b && o.v[(kk + 1) % 3] == a)
						ku = kk;
				if (ku < 0)
					return false;
				const int d = o.v[(ku + 2) % 3];
				if (Orient(verts[a], verts[d], verts[p]) <= 1e-9 || Orient(verts[d], verts[b], verts[p]) <= 1e-9)
					return false; // not convex
				const int t1 = t.n[(k + 1) % 3], t2 = t.n[(k + 2) % 3], u1 = o.n[(ku + 1) % 3], u2 = o.n[(ku + 2) % 3];
				tris[ti].v = {a, d, p}, tris[ti].n = {u1, u, t2};
				tris[u].v = {d, b, p}, tris[u].n = {u2, t1, ti};
				Repoint(u1, u, ti);
				Repoint(t1, ti, u);
				std::vector<int> points = t.points;
				points.insert(points.end(), o.points.begin(), o.points.end());
				Redistribute(points, {ti, u});
				return true;
			}

			// the new point is the corner opposite edge k of ti
			void Legalize(int ti, int k, std::vector<int>& changed, int depth = 0)
			{
				if (depth > 64 || !NeedsFlip(ti, k))
					return;
				const int u = tris[ti].n[k];
				if (!Flip(ti, k))
					return;
				changed.push_back(ti);
				changed.push_back(u);
				Legalize(ti, 0, changed, depth + 1);
				Legalize(u, 0, changed, depth + 1);
			}

			void SplitEdge(int ti, int k, const P3& p, std::vector<int>& changed)
			{
				const Tri t = tris[ti];
				const int a = t.v[k], b = t.v[(k + 1) % 3], c = t.v[(k + 2) % 3];
				const int tBC = t.n[(k + 1) % 3], tCA = t.n[(k + 2) % 3];
				const int u = t.n[k];
				const int q = static_cast<int>(verts.size());
				// the point onto the edge itself (so the outline stays straight)
				const P3& pa = verts[a];
				const P3& pb = verts[b];
				const double dx = pb[0] - pa[0], dy = pb[1] - pa[1];
				const double s = std::clamp(((p[0] - pa[0]) * dx + (p[1] - pa[1]) * dy) / (dx * dx + dy * dy), 0.02, 0.98);
				verts.push_back({pa[0] + s * dx, pa[1] + s * dy, p[2]});
				const int t2 = static_cast<int>(tris.size());
				tris.emplace_back();
				int u2 = -1;
				Tri o;
				int ku = -1;
				if (u >= 0)
				{
					o = tris[u];
					for (int kk = 0; kk < 3; ++kk)
						if (o.v[kk] == b && o.v[(kk + 1) % 3] == a)
							ku = kk;
					if (ku >= 0)
					{
						u2 = static_cast<int>(tris.size());
						tris.emplace_back();
					}
				}
				tris[ti].v = {a, q, c}, tris[ti].n = {u2, t2, tCA};
				tris[t2].v = {q, b, c}, tris[t2].n = {ku >= 0 ? u : -1, tBC, ti};
				Repoint(tBC, ti, t2);
				Redistribute(t.points, {ti, t2});
				changed.insert(changed.end(), {ti, t2});
				if (ku >= 0)
				{
					const int d = o.v[(ku + 2) % 3];
					const int uAD = o.n[(ku + 1) % 3], uDB = o.n[(ku + 2) % 3];
					tris[u].v = {b, q, d}, tris[u].n = {t2, u2, uDB};
					tris[u2].v = {q, a, d}, tris[u2].n = {ti, uAD, u};
					Repoint(uAD, u, u2);
					Redistribute(o.points, {u, u2});
					changed.insert(changed.end(), {u, u2});
					Legalize(u, 2, changed);
					Legalize(u2, 1, changed);
				}
				Legalize(ti, 2, changed);
				Legalize(t2, 1, changed);
			}
		};

		// A plane z = a x + b y + c fitted to points by least squares (sums kept as points come).
		struct PlaneFit
		{
			double sxx = 0, sxy = 0, syy = 0, sx = 0, sy = 0, n = 0, sxz = 0, syz = 0, sz = 0;
			void Add(double x, double y, double z)
			{
				sxx += x * x, sxy += x * y, syy += y * y, sx += x, sy += y, n += 1, sxz += x * z, syz += y * z, sz += z;
			}
			// false when the points do not span a plane
			bool Solve(double& a, double& b, double& c) const
			{
				// | sxx sxy sx | |a|   |sxz|
				// | sxy syy sy | |b| = |syz|
				// | sx  sy  n  | |c|   |sz |
				const double m[3][3] = {{sxx, sxy, sx}, {sxy, syy, sy}, {sx, sy, n}};
				const double r[3] = {sxz, syz, sz};
				const double det = m[0][0] * (m[1][1] * m[2][2] - m[1][2] * m[2][1]) - m[0][1] * (m[1][0] * m[2][2] - m[1][2] * m[2][0]) +
				                   m[0][2] * (m[1][0] * m[2][1] - m[1][1] * m[2][0]);
				if (std::fabs(det) < 1e-9 * std::max(1.0, n * n * n))
					return false;
				const auto d = [&](int col) {
					double t[3][3];
					for (int i = 0; i < 3; ++i)
						for (int j = 0; j < 3; ++j)
							t[i][j] = j == col ? r[i] : m[i][j];
					return t[0][0] * (t[1][1] * t[2][2] - t[1][2] * t[2][1]) - t[0][1] * (t[1][0] * t[2][2] - t[1][2] * t[2][0]) +
					       t[0][2] * (t[1][0] * t[2][1] - t[1][1] * t[2][0]);
				};
				a = d(0) / det, b = d(1) / det, c = d(2) / det;
				return true;
			}
		};

		// The roof's measured heights as planes: cells grown into regions of one plane (normals within 15 degrees, within
		// 0.3 m of the region's plane, refitted as it grows); regions under `minCells` (air conditioners, stair housings,
		// skylights) go to the neighbouring region whose plane is nearest; each cell then takes its region's plane. Curved
		// roofs come out as facets; cells no region takes keep their heights.
		void Planes(std::vector<float>& h, const std::vector<uint8_t>& inside, int nx, int ny, float cell, size_t minCells)
		{
			const size_t n = h.size();
			const auto ok = [&](int i, int j) {
				if (i < 0 || j < 0 || i >= nx || j >= ny)
					return false;
				const size_t k = static_cast<size_t>(j) * nx + i;
				return inside[k] && !std::isnan(h[k]);
			};
			// each cell's own plane (3 x 3) and how well it fits
			std::vector<std::array<float, 3>> normal(n, {0, 0, 1});
			std::vector<float> residual(n, 1e9f);
			for (int j = 0; j < ny; ++j)
				for (int i = 0; i < nx; ++i)
				{
					if (!ok(i, j))
						continue;
					PlaneFit f;
					for (int dj = -1; dj <= 1; ++dj)
						for (int di = -1; di <= 1; ++di)
							if (ok(i + di, j + dj))
								f.Add(di * cell, dj * cell, h[static_cast<size_t>(j + dj) * nx + i + di]);
					double a = 0, b = 0, c = 0;
					if (f.n < 5 || !f.Solve(a, b, c))
						continue;
					const double l = std::sqrt(a * a + b * b + 1);
					const size_t k = static_cast<size_t>(j) * nx + i;
					normal[k] = {static_cast<float>(-a / l), static_cast<float>(-b / l), static_cast<float>(1 / l)};
					double e = 0;
					for (int dj = -1; dj <= 1; ++dj)
						for (int di = -1; di <= 1; ++di)
							if (ok(i + di, j + dj))
							{
								const double d = h[static_cast<size_t>(j + dj) * nx + i + di] - (a * di * cell + b * dj * cell + c);
								e += d * d;
							}
					residual[k] = static_cast<float>(e / f.n);
				}
			std::vector<size_t> order;
			for (size_t k = 0; k < n; ++k)
				if (residual[k] < 1e8f)
					order.push_back(k);
			std::sort(order.begin(), order.end(), [&](size_t a, size_t b) { return residual[a] < residual[b]; });
			std::vector<int> region(n, -1);
			struct Region
			{
				double a = 0, b = 0, c = 0;
				std::array<float, 3> normal{0, 0, 1};
				size_t cells = 0;
			};
			std::vector<Region> regions;
			std::vector<size_t> queue;
			for (const size_t seed : order)
			{
				if (region[seed] >= 0)
					continue;
				const int id = static_cast<int>(regions.size());
				Region r;
				r.normal = normal[seed];
				const double si = static_cast<double>(seed % nx) * cell, sj = static_cast<double>(seed / nx) * cell;
				// (the seed's own plane through its height)
				r.a = -r.normal[0] / r.normal[2], r.b = -r.normal[1] / r.normal[2];
				r.c = h[seed] - r.a * si - r.b * sj;
				PlaneFit f;
				queue.assign(1, seed);
				region[seed] = id;
				size_t refit = 8;
				for (size_t q = 0; q < queue.size(); ++q)
				{
					const size_t k = queue[q];
					const int i = static_cast<int>(k % nx), j = static_cast<int>(k / nx);
					f.Add(i * cell, j * cell, h[k]);
					++r.cells;
					if (r.cells >= refit)
					{
						refit = r.cells * 2;
						double a = 0, b = 0, c = 0;
						if (f.Solve(a, b, c))
						{
							r.a = a, r.b = b, r.c = c;
							const double l = std::sqrt(a * a + b * b + 1);
							r.normal = {static_cast<float>(-a / l), static_cast<float>(-b / l), static_cast<float>(1 / l)};
						}
					}
					for (int d = 0; d < 4; ++d)
					{
						const int ii = i + (d == 0) - (d == 1), jj = j + (d == 2) - (d == 3);
						if (!ok(ii, jj))
							continue;
						const size_t kk = static_cast<size_t>(jj) * nx + ii;
						if (region[kk] >= 0 || residual[kk] > 1e8f)
							continue;
						const auto& m = normal[kk];
						if (m[0] * r.normal[0] + m[1] * r.normal[1] + m[2] * r.normal[2] < 0.966f)
							continue;
						if (std::fabs(h[kk] - (r.a * ii * cell + r.b * jj * cell + r.c)) > 0.3)
							continue;
						region[kk] = id;
						queue.push_back(kk);
					}
				}
				if (r.cells < minCells)
				{
					for (const size_t k : queue)
						region[k] = -2; // (too small: given to a neighbour below)
					r.cells = 0;
				}
				regions.push_back(r);
			}
			// the small ones and the cells no region took: to the neighbouring region whose plane is nearest
			for (int pass = 0; pass < 200; ++pass)
			{
				std::vector<std::pair<size_t, int>> take;
				for (int j = 0; j < ny; ++j)
					for (int i = 0; i < nx; ++i)
					{
						const size_t k = static_cast<size_t>(j) * nx + i;
						if (!ok(i, j) || region[k] >= 0)
							continue;
						int best = -1;
						double bestD = 1e30;
						for (int d = 0; d < 4; ++d)
						{
							const int ii = i + (d == 0) - (d == 1), jj = j + (d == 2) - (d == 3);
							if (!ok(ii, jj))
								continue;
							const int id = region[static_cast<size_t>(jj) * nx + ii];
							if (id < 0)
								continue;
							const Region& r = regions[id];
							const double dist = std::fabs(h[k] - (r.a * i * cell + r.b * j * cell + r.c));
							if (dist < bestD)
								bestD = dist, best = id;
						}
						if (best >= 0)
							take.push_back({k, best});
					}
				if (take.empty())
					break;
				for (const auto& [k, id] : take)
					region[k] = id;
			}
			for (size_t k = 0; k < n; ++k)
				if (region[k] >= 0)
				{
					const Region& r = regions[region[k]];
					h[k] = static_cast<float>(r.a * static_cast<double>(k % nx) * cell + r.b * static_cast<double>(k / nx) * cell + r.c);
				}
		}

		float Percentile(std::vector<float> v, float q)
		{
			if (v.empty())
				return std::numeric_limits<float>::quiet_NaN();
			const size_t k = std::min(v.size() - 1, static_cast<size_t>(q * (v.size() - 1) + 0.5f));
			std::nth_element(v.begin(), v.begin() + static_cast<ptrdiff_t>(k), v.end());
			return v[k];
		}
	}

	bool Make(const world::Building& b, elevation::Scan* scan, const std::function<float(float, float)>& ground, const Options& o, Shape& out,
	    Stats& stats)
	{
		out = {};
		out.kind = b.kind;
		const std::vector<P2> ring = polygon::Clean(b.footprint, 0.05f);
		if (ring.size() < 3 || polygon::Area(ring) < 4)
			return ++stats.skipped, false;
		// the base: the lowest ground along the outline
		float base = std::numeric_limits<float>::max();
		for (size_t i = 0; i < ring.size(); ++i)
		{
			const P2& p = ring[i];
			const P2& q = ring[(i + 1) % ring.size()];
			const int steps = std::max(1, static_cast<int>(std::hypot(q[0] - p[0], q[1] - p[1]) / 4));
			for (int s = 0; s < steps; ++s)
				base = std::min(base, ground(p[0] + (q[0] - p[0]) * s / steps, p[1] + (q[1] - p[1]) * s / steps));
		}
		out.base = base;

		// the measured roof over the footprint: 0.5 m cells, building returns; holes (trees over the roof, no returns)
		// filled from around them; open ground well inside the outline is a courtyard
		float minX = 1e30f, minY = 1e30f, maxX = -1e30f, maxY = -1e30f;
		for (const P2& p : ring)
			minX = std::min(minX, p[0]), minY = std::min(minY, p[1]), maxX = std::max(maxX, p[0]), maxY = std::max(maxY, p[1]);
		const float cell = scan ? scan->Cell() : 0.5f;
		const float gx0 = std::floor(minX / cell) * cell, gy0 = std::floor(minY / cell) * cell;
		const int nx = static_cast<int>(std::ceil((maxX - gx0) / cell)), ny = static_cast<int>(std::ceil((maxY - gy0) / cell));
		const float nan = std::numeric_limits<float>::quiet_NaN();
		std::vector<float> h(static_cast<size_t>(nx) * ny, nan);
		std::vector<uint8_t> inside(h.size(), 0);
		std::vector<float> edgeDistance(h.size(), 0);
		size_t insideCount = 0, known = 0;
		// (a scan with no returns over the footprint - past its edge: one over part of the land - is none)
		bool scanned = false;
		if (scan && !scan->Empty())
		{
			P2 mid{0, 0};
			for (const P2& p : ring)
				mid[0] += p[0] / ring.size(), mid[1] += p[1] / ring.size();
			scanned = scan->Count(mid[0], mid[1]) > 0;
			for (size_t e = 0; e < ring.size() && !scanned; ++e)
				scanned = scan->Count(ring[e][0], ring[e][1]) > 0;
		}
		// (a building site's shape not measured: the scan is years older than the map - a half-built block's returns had made
		// a roof of spikes and shards, the reviewers' "broken building mesh"; it gets the map's height, flat)
		const bool site = b.kind == "construction";
		const bool measure = scanned && !b.planned && !site && nx * ny < 4000000;
		// (no measurement: a house's hip roof - each cell's height up from the nearest wall)
		const double area = std::fabs(polygon::Area(ring));
		const bool houseKind = b.kind == "house" || b.kind == "detached" || b.kind == "residential" || b.kind == "semidetached_house" ||
		                       b.kind == "bungalow" || b.kind == "terrace" || b.kind == "cabin" || b.kind == "farm" || b.kind == "static_caravan" ||
		                       ((b.kind == "yes" || b.kind.empty()) && area >= 50 && area <= 350 && !(o.attached && o.attached->contains(&b)));
		const bool lowRise = (b.kind == "apartments" || b.kind == "dormitory") && b.height <= 15 && area <= 3000;
		bool hip = !measure && !b.planned && !site && ((houseKind && area <= 700 && b.height <= 12) || lowRise) && nx * ny < 400000;
		// (a house the scan measured gets the same clean roof, at the eaves and ridge it measured: its lumpy surface with the
		// aerial picture on it was a white blanket in the street pictures' comparison, not a roof)
		const bool measuredHouse = measure && !b.planned && houseKind && area <= 700 && b.height <= 12 && nx * ny < 400000;
		// (a house's roof at 0.55 - 29 degrees, north Florida's cottages' 6/12 to 8/12 -, its ridge at most 4 m up: at 0.42 the
		// roofs had hardly shown from the street, "flat roofs with a white fascia")
		float eave = b.height > 7.5f ? b.height - 2.5f : 3.0f, riseCap = 4.0f;
		// (a gable roof instead: on a footprint near its narrowest box - 85 % of it and more -, by the place's own number;
		// the height then rises only from the walls along the long side, the short sides' walls - their edges across the
		// ridge - becoming the gables)
		std::vector<bool> eaveEdge(ring.size(), true);
		if ((hip || measuredHouse) && o.gableShare > 0)
		{
			float bestArea = 1e30f, axis = 0;
			for (size_t e = 0; e < ring.size(); ++e)
			{
				const P2& p = ring[e];
				const P2& q = ring[(e + 1) % ring.size()];
				const float len = std::hypot(q[0] - p[0], q[1] - p[1]);
				if (len < 0.5f)
					continue;
				const float ux = (q[0] - p[0]) / len, uy = (q[1] - p[1]) / len;
				float u0 = 1e30f, u1 = -1e30f, v0 = 1e30f, v1 = -1e30f;
				for (const P2& r : ring)
				{
					const float u = r[0] * ux + r[1] * uy, v = -r[0] * uy + r[1] * ux;
					u0 = std::min(u0, u), u1 = std::max(u1, u), v0 = std::min(v0, v), v1 = std::max(v1, v);
				}
				if ((u1 - u0) * (v1 - v0) < bestArea)
					bestArea = (u1 - u0) * (v1 - v0), axis = u1 - u0 >= v1 - v0 ? std::atan2(uy, ux) : std::atan2(ux, -uy);
			}
			uint32_t seed = static_cast<uint32_t>(std::lround(ring[0][0] * 7)) * 73856093u ^ static_cast<uint32_t>(std::lround(ring[0][1] * 7)) * 19349663u;
			seed ^= seed >> 15, seed *= 0x2c1b3c6du, seed ^= seed >> 12;
			if (area >= 0.85 * bestArea && static_cast<float>(seed % 1000) < o.gableShare * 1000)
			{
				const float ax = std::cos(axis), ay = std::sin(axis);
				for (size_t e = 0; e < ring.size(); ++e)
				{
					const P2& p = ring[e];
					const P2& q = ring[(e + 1) % ring.size()];
					const float len = std::hypot(q[0] - p[0], q[1] - p[1]);
					// (an edge along the ridge - within 30 degrees - carries an eave; one across it is a gable)
					eaveEdge[e] = len > 1e-3f && std::fabs(((q[0] - p[0]) * ax + (q[1] - p[1]) * ay) / len) > 0.866f;
				}
			}
		}
		// the heights over the footprint: 0 a hip roof's, 1 the scan's building returns, 2 its highest returns of any kind
		const auto fill = [&](int mode) {
			std::fill(h.begin(), h.end(), nan);
			std::fill(inside.begin(), inside.end(), uint8_t{0});
			insideCount = 0, known = 0;
			for (int j = 0; j < ny; ++j)
				for (int i = 0; i < nx; ++i)
				{
					const P2 c{gx0 + (i + 0.5f) * cell, gy0 + (j + 0.5f) * cell};
					if (!polygon::Inside(ring, c))
						continue;
					const size_t k = static_cast<size_t>(j) * nx + i;
					inside[k] = 1;
					++insideCount;
					float d = 1e30f, de = 1e30f;
					for (size_t e = 0; e < ring.size(); ++e)
					{
						const float de1 = polygon::DistanceToSegment(c, ring[e], ring[(e + 1) % ring.size()]);
						d = std::min(d, de1);
						if (eaveEdge[e])
							de = std::min(de, de1);
					}
					edgeDistance[k] = d;
					if (mode == 0)
					{
						h[k] = base + eave + std::min((de < 1e29f ? de : d) * kHouseSlope, riseCap), ++known;
						continue;
					}
					const float roof = mode == 1 ? scan->Roof(c[0], c[1]) : scan->Top(c[0], c[1]);
					if (!std::isnan(roof) && (mode == 1 || d > 0.5f))
					{
						h[k] = roof, ++known;
						continue;
					}
					const float top = scan->Top(c[0], c[1]), g = scan->Ground(c[0], c[1]);
					if (d > 1.5f && !std::isnan(top) && !std::isnan(g) && top - g < 0.8f)
						h[k] = g, ++known;
				}
		};
		if (measure || hip)
			fill(hip ? 0 : 1);
		if (measuredHouse && insideCount >= 8 && static_cast<float>(known) >= o.coverage * static_cast<float>(insideCount))
		{
			// (its eaves: the low fifth of the heights along the walls; its ridge: the high twentieth)
			std::vector<float> edge, all;
			for (size_t k = 0; k < h.size(); ++k)
				if (inside[k] && !std::isnan(h[k]))
				{
					all.push_back(h[k]);
					if (edgeDistance[k] < 1.0f)
						edge.push_back(h[k]);
				}
			if (edge.size() >= 4 && all.size() >= 8)
			{
				std::sort(edge.begin(), edge.end());
				std::sort(all.begin(), all.end());
				const float eaveZ = edge[edge.size() / 5], ridgeZ = all[all.size() * 19 / 20];
				eave = std::clamp(eaveZ - base, 2.4f, 9.0f);
				// (its ridge at a house's pitch, at least 2 m over the eaves: the scan's ridge, noisy over a small roof, had
				// made the roofs flat boxes in the reviewers' eyes)
				riseCap = std::clamp(ridgeZ - eaveZ, 2.0f, 5.0f);
				hip = true;
				fill(0);
			}
		}
		// (a measurement short of its coverage - too few of the scan's building returns -: a house gets its hip roof after all
		// - a flat box had stood where the street pictures have a pitched roof -, a big structure its shape by the scan's
		// highest returns of any kind - a stadium's open stands came as unclassified returns: a low box at the map's guess)
		if (measure && !hip && (insideCount < 8 || static_cast<float>(known) < o.coverage * static_cast<float>(insideCount)))
		{
			if (houseKind && area <= 700 && b.height <= 12)
				hip = true, fill(0);
			else if (area >= 1500)
				fill(2);
		}
		if (!(measure || hip) || insideCount < 8 || static_cast<float>(known) < o.coverage * static_cast<float>(insideCount))
		{
			// the map's height, a flat roof
			++stats.mapHeight;
			Tin tin;
			const float z = base + std::max(2.5f, b.height);
			if (!tin.Init(ring, std::vector<double>(ring.size(), z)))
				return ++stats.skipped, false;
			for (const P3& v : tin.verts)
				out.vertices.push_back(v);
			for (const auto& t : tin.tris)
			{
				out.roof.push_back({static_cast<uint32_t>(t.v[0]), static_cast<uint32_t>(t.v[1]), static_cast<uint32_t>(t.v[2])});
				for (int k = 0; k < 3; ++k)
					if (t.n[k] < 0)
						out.outline.push_back({static_cast<uint32_t>(t.v[k]), static_cast<uint32_t>(t.v[(k + 1) % 3])});
			}
			out.height = out.top = z - base;
			stats.triangles += out.roof.size();
			return true;
		}
		if (hip)
			++stats.pitched, out.pitched = true;
		else
			++stats.measured, out.measured = true;
		// holes: the mean of known neighbours, outwards
		for (int pass = 0; pass < 400; ++pass)
		{
			std::vector<std::pair<size_t, float>> fill;
			for (int j = 0; j < ny; ++j)
				for (int i = 0; i < nx; ++i)
				{
					const size_t k = static_cast<size_t>(j) * nx + i;
					if (!inside[k] || !std::isnan(h[k]))
						continue;
					float s = 0;
					int n = 0;
					for (int dj = -1; dj <= 1; ++dj)
						for (int di = -1; di <= 1; ++di)
						{
							const int ii = i + di, jj = j + dj;
							if (ii < 0 || jj < 0 || ii >= nx || jj >= ny)
								continue;
							const float v = h[static_cast<size_t>(jj) * nx + ii];
							if (!std::isnan(v))
								s += v, ++n;
						}
					if (n)
						fill.push_back({k, s / n});
				}
			if (fill.empty())
				break;
			for (const auto& [k, v] : fill)
				h[k] = v;
		}
		// a 3 x 3 median (aerials, rails, single returns off antennas)
		{
			std::vector<float> m = h;
			std::vector<float> w;
			for (int j = 0; j < ny; ++j)
				for (int i = 0; i < nx; ++i)
				{
					const size_t k = static_cast<size_t>(j) * nx + i;
					if (!inside[k])
						continue;
					w.clear();
					for (int dj = -1; dj <= 1; ++dj)
						for (int di = -1; di <= 1; ++di)
						{
							const int ii = i + di, jj = j + dj;
							if (ii < 0 || jj < 0 || ii >= nx || jj >= ny)
								continue;
							const size_t kk = static_cast<size_t>(jj) * nx + ii;
							if (inside[kk] && !std::isnan(h[kk]))
								w.push_back(h[kk]);
						}
					if (!w.empty())
					{
						std::nth_element(w.begin(), w.begin() + static_cast<ptrdiff_t>(w.size() / 2), w.end());
						m[k] = w[w.size() / 2];
					}
				}
			h = std::move(m);
		}
		Planes(h, inside, nx, ny, cell, static_cast<size_t>(o.minPlane / (cell * cell)));
		std::vector<float> all, middle;
		for (size_t k = 0; k < h.size(); ++k)
			if (inside[k] && !std::isnan(h[k]))
			{
				all.push_back(h[k]);
				if (edgeDistance[k] > 1.0f)
					middle.push_back(h[k]);
			}
		if (middle.size() < all.size() / 3)
			middle = all;
		const float median = Percentile(middle, 0.5f);
		const bool flat = Percentile(middle, 0.9f) - Percentile(middle, 0.1f) < o.flat;
		// a height at any point: bilinear over the cells inside (the nearest inside cell when none of the four is)
		const auto heightAt = [&](float x, float y) -> double {
			const float fx = (x - gx0) / cell - 0.5f, fy = (y - gy0) / cell - 0.5f;
			const int i0 = static_cast<int>(std::floor(fx)), j0 = static_cast<int>(std::floor(fy));
			const float tx = fx - i0, ty = fy - j0;
			double s = 0, w = 0;
			for (int k = 0; k < 4; ++k)
			{
				const int i = i0 + (k & 1), j = j0 + (k >> 1);
				if (i < 0 || j < 0 || i >= nx || j >= ny)
					continue;
				const size_t kk = static_cast<size_t>(j) * nx + i;
				if (!inside[kk] || std::isnan(h[kk]))
					continue;
				const double wk = ((k & 1) ? tx : 1 - tx) * ((k >> 1) ? ty : 1 - ty) + 1e-6;
				s += wk * h[kk], w += wk;
			}
			if (w > 0)
				return s / w;
			double best = 1e30, v = median;
			for (int j = std::max(0, j0 - 4); j <= std::min(ny - 1, j0 + 5); ++j)
				for (int i = std::max(0, i0 - 4); i <= std::min(nx - 1, i0 + 5); ++i)
				{
					const size_t kk = static_cast<size_t>(j) * nx + i;
					if (!inside[kk] || std::isnan(h[kk]))
						continue;
					const double d = std::hypot(gx0 + (i + 0.5) * cell - x, gy0 + (j + 0.5) * cell - y);
					if (d < best)
						best = d, v = h[kk];
				}
			return v;
		};
		// the outline's corners: measured a little inside (the bisector), or the median of a flat roof
		std::vector<double> corner(ring.size(), median);
		if (!flat)
			for (size_t i = 0; i < ring.size(); ++i)
			{
				const P2& p = ring[i];
				const P2& a = ring[(i + ring.size() - 1) % ring.size()];
				const P2& c = ring[(i + 1) % ring.size()];
				float ux = p[0] - a[0], uy = p[1] - a[1], vx = c[0] - p[0], vy = c[1] - p[1];
				const float lu = std::hypot(ux, uy), lv = std::hypot(vx, vy);
				ux /= lu, uy /= lu, vx /= lv, vy /= lv;
				// (inward: left of each edge, anticlockwise)
				float bx = -uy - vy, by = ux + vx;
				const float lb = std::hypot(bx, by);
				if (lb > 1e-4f)
					bx /= lb, by /= lb;
				corner[i] = heightAt(p[0] + bx * 0.5f, p[1] + by * 0.5f);
			}
		Tin tin;
		if (!tin.Init(ring, corner))
			return ++stats.skipped, false;
		if (flat)
			++stats.flat;
		else
		{
			++stats.shaped;
			// candidates: the cells' centres away from the outline, and points along it every half metre
			for (int j = 0; j < ny; ++j)
				for (int i = 0; i < nx; ++i)
				{
					const size_t k = static_cast<size_t>(j) * nx + i;
					if (inside[k] && edgeDistance[k] > 0.35f && !std::isnan(h[k]))
						tin.cands.push_back({gx0 + (i + 0.5) * cell, gy0 + (j + 0.5) * cell, h[k]});
				}
			for (size_t e = 0; e < ring.size(); ++e)
			{
				const P2& p = ring[e];
				const P2& q = ring[(e + 1) % ring.size()];
				const float len = std::hypot(q[0] - p[0], q[1] - p[1]);
				const float nx2 = -(q[1] - p[1]) / len, ny2 = (q[0] - p[0]) / len;
				const int steps = static_cast<int>(len / 0.5f);
				for (int s = 1; s < steps; ++s)
				{
					const float t = static_cast<float>(s) / steps;
					const float x = p[0] + (q[0] - p[0]) * t, y = p[1] + (q[1] - p[1]) * t;
					tin.cands.push_back({x, y, heightAt(x + nx2 * 0.35f, y + ny2 * 0.35f)});
				}
			}
			tin.Distribute();
			using Item = std::tuple<double, int, int, int>; // error, triangle, candidate, stamp
			std::priority_queue<Item> heap;
			const auto push = [&](int ti) {
				double e = 0;
				const int c = tin.Worst(ti, e);
				if (c >= 0)
					heap.push({e, ti, c, tin.tris[ti].stamp});
			};
			for (int ti = 0; ti < static_cast<int>(tin.tris.size()); ++ti)
				push(ti);
			std::vector<int> changed;
			// (a big roof more triangles - one every 2.5 m² over 800 -: a set-back tower's terraces had run out of them, their
			// steps drawn as slopes metres wide with the walls' glass on them, the reviewers' "glass wedge floating halfway up
			// the facade")
			const size_t cap = std::max(o.maxTriangles, static_cast<size_t>(area / 2.5));
			while (!heap.empty() && tin.tris.size() < cap)
			{
				const auto [e, ti, c, stamp] = heap.top();
				heap.pop();
				if (tin.tris[ti].stamp != stamp)
					continue;
				if (e <= o.tolerance)
					break;
				changed.clear();
				tin.Insert(ti, c, changed);
				std::sort(changed.begin(), changed.end());
				changed.erase(std::unique(changed.begin(), changed.end()), changed.end());
				for (const int t : changed)
				{
					++tin.tris[t].stamp;
					push(t);
				}
			}
		}
		double top = -1e30;
		for (const P3& v : tin.verts)
		{
			out.vertices.push_back(v);
			top = std::max(top, v[2]);
		}
		for (const auto& t : tin.tris)
		{
			if (!t.alive)
				continue;
			out.roof.push_back({static_cast<uint32_t>(t.v[0]), static_cast<uint32_t>(t.v[1]), static_cast<uint32_t>(t.v[2])});
			for (int k = 0; k < 3; ++k)
				if (t.n[k] < 0)
					out.outline.push_back({static_cast<uint32_t>(t.v[k]), static_cast<uint32_t>(t.v[(k + 1) % 3])});
		}
		out.height = median - base;
		out.top = static_cast<float>(top) - base;
		stats.triangles += out.roof.size();
		return !out.roof.empty();
	}

	namespace
	{
		// Douglas-Peucker on a closed ring (split at its two farthest-apart points).
		std::vector<P2> Simplify(const std::vector<P2>& ring, float tolerance)
		{
			if (ring.size() < 4)
				return ring;
			size_t far = 0;
			float best = -1;
			for (size_t i = 1; i < ring.size(); ++i)
				if (const float d = std::hypot(ring[i][0] - ring[0][0], ring[i][1] - ring[0][1]); d > best)
					best = d, far = i;
			std::vector<bool> keep(ring.size(), false);
			keep[0] = keep[far] = true;
			const auto split = [&](auto& self, size_t a, size_t b) -> void {
				// (the points from a to b round the ring)
				const size_t n = ring.size();
				size_t worst = a;
				float worstD = 0;
				for (size_t k = (a + 1) % n; k != b; k = (k + 1) % n)
					if (const float d = polygon::DistanceToSegment(ring[k], ring[a], ring[b]); d > worstD)
						worstD = d, worst = k;
				if (worstD > tolerance)
				{
					keep[worst] = true;
					self(self, a, worst);
					self(self, worst, b);
				}
			};
			split(split, 0, far);
			split(split, far, 0);
			std::vector<P2> out;
			for (size_t i = 0; i < ring.size(); ++i)
				if (keep[i])
					out.push_back(ring[i]);
			return out;
		}
	}

	std::vector<world::Building> Missing(const std::function<void(elevation::Scan&)>& open, const std::vector<world::Building>& known,
	    const std::array<float, 4>& box, float minArea)
	{
		constexpr float kCell = 0.5f, kBlock = 1000, kMargin = 60;
		// (the known footprints by 250 m cell of their first point, to find those near a block)
		std::map<std::pair<int, int>, std::vector<const world::Building*>> byCell;
		for (const auto& b : known)
			if (b.footprint.size() >= 3)
				byCell[{static_cast<int>(std::floor(b.footprint[0][0] / 250)), static_cast<int>(std::floor(b.footprint[0][1] / 250))}].push_back(&b);
		// (the blocks on all the cores, each worker with a scan reader of its own; the pieces put together in the blocks'
		// order, as in turn)
		std::vector<std::pair<float, float>> blocks;
		for (float by = box[1]; by < box[3]; by += kBlock)
			for (float bx = box[0]; bx < box[2]; bx += kBlock)
				blocks.push_back({bx, by});
		std::vector<std::vector<world::Building>> found(blocks.size());
		std::atomic<size_t> nextBlock = 0;
		std::vector<std::thread> workers;
		for (unsigned k = 0; k < std::max(1u, std::thread::hardware_concurrency()); ++k)
			workers.emplace_back([&] {
				elevation::Scan scan;
				open(scan);
				for (size_t block; (block = nextBlock++) < blocks.size();)
				{
					const auto [bx, by] = blocks[block];
					std::vector<world::Building>& out = found[block];
						const float cx1 = std::min(bx + kBlock, box[2]), cy1 = std::min(by + kBlock, box[3]);
						raster::Grid<uint8_t> g; // the scan's building cells
						g.Init(bx - kMargin, by - kMargin, cx1 + kMargin, cy1 + kMargin, kCell, 0);
						bool any = false;
						for (int j = 0; j < g.ny; ++j)
							for (int i = 0; i < g.nx; ++i)
							{
								const P2 c = g.Centre(i, j);
								if (scan.Flags(c[0], c[1]) & elevation::kBuilding)
									g.At(i, j) = 1, any = true;
							}
						if (!any)
							continue;
						raster::Grid<uint8_t> covered = g;
						std::fill(covered.v.begin(), covered.v.end(), uint8_t{0});
						for (int cj = static_cast<int>(std::floor((by - kMargin - 300) / 250)); cj <= static_cast<int>(std::floor((cy1 + kMargin + 300) / 250)); ++cj)
							for (int ci = static_cast<int>(std::floor((bx - kMargin - 300) / 250)); ci <= static_cast<int>(std::floor((cx1 + kMargin + 300) / 250)); ++ci)
								if (const auto it = byCell.find({ci, cj}); it != byCell.end())
									for (const world::Building* b : it->second)
										raster::FillPolygon(covered, {b->footprint}, uint8_t{1});
						// (grown or shrunk a cell at a time, 4-neighbours)
						const auto grow = [&](const std::vector<uint8_t>& from, int steps, bool erode) {
							std::vector<uint8_t> cur = from;
							for (int k = 0; k < steps; ++k)
							{
								std::vector<uint8_t> next = cur;
								for (int j = 0; j < g.ny; ++j)
									for (int i = 0; i < g.nx; ++i)
									{
										const size_t at = static_cast<size_t>(j) * g.nx + i;
										const bool on = cur[at] != 0;
										if (erode ? !on : on)
											continue;
										for (const auto& [di, dj] : {std::pair{1, 0}, std::pair{-1, 0}, std::pair{0, 1}, std::pair{0, -1}})
										{
											const int ii = i + di, jj = j + dj;
											const bool other = ii >= 0 && jj >= 0 && ii < g.nx && jj < g.ny && cur[static_cast<size_t>(jj) * g.nx + ii] != 0;
											if (erode ? !other : other)
											{
												next[at] = erode ? 0 : 1;
												break;
											}
										}
									}
								cur = std::move(next);
							}
							return cur;
						};
						// a metre of slack round the footprints, then the opening (3 cells in, 3 out)
						const auto slack = grow(covered.v, 2, false);
						std::vector<uint8_t> loose(g.v.size());
						for (size_t k = 0; k < loose.size(); ++k)
							loose[k] = g.v[k] && !slack[k];
						const auto opened = grow(grow(loose, 3, true), 3, false);
						// the pieces
						std::vector<int> label(opened.size(), -1);
						int pieces = 0;
						for (int j0 = 0; j0 < g.ny; ++j0)
							for (int i0 = 0; i0 < g.nx; ++i0)
							{
								const size_t start = static_cast<size_t>(j0) * g.nx + i0;
								if (!opened[start] || label[start] >= 0)
									continue;
								const int id = pieces++;
								std::vector<std::pair<int, int>> cells, stack{{i0, j0}};
								label[start] = id;
								while (!stack.empty())
								{
									const auto [i, j] = stack.back();
									stack.pop_back();
									cells.push_back({i, j});
									for (const auto& [di, dj] : {std::pair{1, 0}, std::pair{-1, 0}, std::pair{0, 1}, std::pair{0, -1}})
									{
										const int ii = i + di, jj = j + dj;
										if (ii < 0 || jj < 0 || ii >= g.nx || jj >= g.ny)
											continue;
										const size_t at = static_cast<size_t>(jj) * g.nx + ii;
										if (opened[at] && label[at] < 0)
											label[at] = id, stack.push_back({ii, jj});
									}
								}
								const P2 first = g.Centre(i0, j0);
								if (cells.size() * kCell * kCell < minArea || first[0] < bx || first[0] >= cx1 || first[1] < by || first[1] >= cy1)
									continue;
								// its outline: the cells' outer edges chained anticlockwise (corners in cell units; an edge is the
								// piece's where the cell beyond it is not)
								const auto in = [&](int i, int j) { return i >= 0 && j >= 0 && i < g.nx && j < g.ny && label[static_cast<size_t>(j) * g.nx + i] == id; };
								std::map<std::pair<int, int>, std::pair<int, int>> next;
								for (const auto& [i, j] : cells)
								{
									if (!in(i, j - 1))
										next[{i, j}] = {i + 1, j};
									if (!in(i + 1, j))
										next[{i + 1, j}] = {i + 1, j + 1};
									if (!in(i, j + 1))
										next[{i + 1, j + 1}] = {i, j + 1};
									if (!in(i - 1, j))
										next[{i, j + 1}] = {i, j};
								}
								// (the outer ring: from the lowest, leftmost corner)
								std::pair<int, int> at = next.begin()->first;
								for (const auto& [corner, to] : next)
									if (corner.second < at.second || (corner.second == at.second && corner.first < at.first))
										at = corner;
								std::vector<P2> ring;
								const auto begin = at;
								for (size_t guard = 0; guard <= next.size(); ++guard)
								{
									ring.push_back({g.x0 + at.first * kCell, g.y0 + at.second * kCell});
									const auto it = next.find(at);
									if (it == next.end())
										break;
									at = it->second;
									if (at == begin)
										break;
								}
								ring = Simplify(polygon::Clean(ring, 0.05f), 0.6f);
								if (ring.size() < 3 || polygon::Area(ring) < minArea * 0.6)
									continue;
								// its height: the middle of the scan's roof over it above the ground beside it
								std::vector<float> heights;
								for (size_t k = 0; k < cells.size(); k += 4)
								{
									const P2 c = g.Centre(cells[k].first, cells[k].second);
									const float roof = scan.Roof(c[0], c[1]), ground = scan.Ground(c[0] + 3, c[1] + 3);
									if (!std::isnan(roof) && !std::isnan(ground))
										heights.push_back(roof - ground);
								}
								// (lower than a storey: cars, containers, stacked material on a site - not a building)
								if (heights.empty())
									continue;
								std::nth_element(heights.begin(), heights.begin() + heights.size() / 2, heights.end());
								if (heights[heights.size() / 2] < 2.8f)
									continue;
								world::Building b;
								b.height = heights[heights.size() / 2];
								b.footprint = std::move(ring);
								b.kind = "yes";
								out.push_back(std::move(b));
							}
				}
			});
		for (auto& worker : workers)
			worker.join();
		std::vector<world::Building> out;
		for (auto& f : found)
			out.insert(out.end(), std::make_move_iterator(f.begin()), std::make_move_iterator(f.end()));
		return out;
	}
}
