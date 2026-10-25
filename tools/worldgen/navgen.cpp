#include "navgen.hpp"

#include <Recast.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstring>
#include <format>
#include <limits>
#include <set>

namespace worldgen::navgen
{
	namespace
	{
		using Vec = std::array<float, 3>;
		constexpr float kSquare = 150.0f, kOrigin = -6000.0f;
		constexpr uint32_t kNone = 0x000FFFE1; // no polygon across: polygon 0x7FFF, slot 1 (the adjacency table's 0x3FFF)
		constexpr uint32_t kTag = 0x7F800001;  // what the game keeps in a vector's fourth float
		constexpr uint32_t kPolysPerChunk = 341;
		constexpr float kSideTolerance = 0.6f; // (m) how far apart in height two squares' vertices on their side may be

		struct Polygon
		{
			std::vector<Vec> v; // anticlockwise seen from above
			Surface surface = Surface::None;
			std::vector<std::pair<int, int>> across; // per edge v[k] -> v[k + 1]: (square, polygon) across, or (-1, -1)
		};

		struct SquareMesh
		{
			Square square;
			std::vector<Polygon> polys;
		};

		class Context : public rcContext
		{
		public:
			std::string errors;

		protected:
			void doLog(const rcLogCategory category, const char* message, const int length) override
			{
				if (category == RC_LOG_ERROR)
					errors.append(message, length).append("; ");
			}
		};

		float SideX(int square, int side) { return kOrigin + kSquare * (square + side); }

		// One square: the walkable polygons of the triangles that reach into it (and its border).
		bool BuildSquare(const bounds::Mesh& mesh, const std::vector<uint8_t>& areas, const std::vector<std::array<float, 4>>& boxes,
		    const std::vector<uint32_t>& near, const Settings& s, const Square& square, std::vector<Polygon>& out, std::string& error)
		{
			const float x0 = SideX(square.first, 0), y0 = SideX(square.second, 0);
			const int radius = static_cast<int>(std::ceil(s.agentRadius / s.cellSize));
			const int border = radius + 3;
			const float pad = border * s.cellSize;
			// the triangles reaching into the square and its border, in Recast's space (x, height, -y: a rotation, so
			// every face keeps its winding)
			std::vector<float> verts;
			std::vector<int> tris;
			std::vector<uint8_t> triAreas;
			float zMin = std::numeric_limits<float>::max(), zMax = -std::numeric_limits<float>::max();
			for (const uint32_t t : near)
			{
				const auto& b = boxes[t];
				if (!areas[t] || b[1] < x0 - pad || b[0] > x0 + kSquare + pad || b[3] < y0 - pad || b[2] > y0 + kSquare + pad)
					continue;
				for (const uint32_t i : mesh.triangles[t])
				{
					const Vec& p = mesh.vertices[i];
					tris.push_back(static_cast<int>(verts.size() / 3));
					verts.insert(verts.end(), {p[0], p[2], -p[1]});
					zMin = std::min(zMin, p[2]);
					zMax = std::max(zMax, p[2]);
				}
				triAreas.push_back(areas[t]);
			}
			// (obstacles: the triangles nobody walks on still stand in the way)
			for (const uint32_t t : near)
			{
				const auto& b = boxes[t];
				if (areas[t] || b[1] < x0 - pad || b[0] > x0 + kSquare + pad || b[3] < y0 - pad || b[2] > y0 + kSquare + pad)
					continue;
				for (const uint32_t i : mesh.triangles[t])
				{
					const Vec& p = mesh.vertices[i];
					tris.push_back(static_cast<int>(verts.size() / 3));
					verts.insert(verts.end(), {p[0], p[2], -p[1]});
				}
				triAreas.push_back(RC_NULL_AREA);
			}
			if (zMin > zMax)
				return true; // nothing walkable reaches the square
			const int nt = static_cast<int>(triAreas.size()), nv = static_cast<int>(verts.size() / 3);
			// the slope limit (walkable areas of steeper faces cleared)
			rcClearUnwalkableTriangles(nullptr, s.maxSlope, verts.data(), nv, tris.data(), nt, triAreas.data());

			const float range = zMax - zMin + s.agentHeight + 2.0f;
			const float ch = std::max(s.cellHeight, range / 8000.0f); // (Recast's spans: 13 bits of height)
			rcConfig c{};
			c.cs = s.cellSize;
			c.ch = ch;
			c.walkableSlopeAngle = s.maxSlope;
			c.walkableHeight = static_cast<int>(std::ceil(s.agentHeight / ch));
			c.walkableClimb = static_cast<int>(std::floor(s.agentClimb / ch));
			c.walkableRadius = radius;
			c.maxEdgeLen = static_cast<int>(s.maxEdgeLength / s.cellSize);
			c.maxSimplificationError = s.maxError / s.cellSize;
			c.minRegionArea = 8 * 8;
			c.mergeRegionArea = 20 * 20;
			c.maxVertsPerPoly = s.maxVertsPerPoly;
			c.tileSize = static_cast<int>(kSquare / s.cellSize);
			c.borderSize = border;
			c.width = c.height = c.tileSize + 2 * border;
			c.bmin[0] = x0 - pad;
			c.bmin[1] = zMin - 1.0f;
			c.bmin[2] = -(y0 + kSquare) - pad;
			c.bmax[0] = x0 + kSquare + pad;
			c.bmax[1] = zMin - 1.0f + range;
			c.bmax[2] = -y0 + pad;

			Context ctx;
			struct Free
			{
				rcHeightfield* hf = nullptr;
				rcCompactHeightfield* chf = nullptr;
				rcContourSet* cset = nullptr;
				rcPolyMesh* pmesh = nullptr;
				~Free()
				{
					rcFreeHeightField(hf);
					rcFreeCompactHeightfield(chf);
					rcFreeContourSet(cset);
					rcFreePolyMesh(pmesh);
				}
			} r;
			r.hf = rcAllocHeightfield();
			r.chf = rcAllocCompactHeightfield();
			r.cset = rcAllocContourSet();
			r.pmesh = rcAllocPolyMesh();
			if (!r.hf || !r.chf || !r.cset || !r.pmesh)
				return error = "out of memory", false;
			const bool ok = rcCreateHeightfield(&ctx, *r.hf, c.width, c.height, c.bmin, c.bmax, c.cs, c.ch) &&
			                rcRasterizeTriangles(&ctx, verts.data(), nv, tris.data(), triAreas.data(), nt, *r.hf, c.walkableClimb) &&
			                [&] {
				                rcFilterLowHangingWalkableObstacles(&ctx, c.walkableClimb, *r.hf);
				                rcFilterLedgeSpans(&ctx, c.walkableHeight, c.walkableClimb, *r.hf);
				                rcFilterWalkableLowHeightSpans(&ctx, c.walkableHeight, *r.hf);
				                return true;
			                }() &&
			                rcBuildCompactHeightfield(&ctx, c.walkableHeight, c.walkableClimb, *r.hf, *r.chf) &&
			                rcErodeWalkableArea(&ctx, c.walkableRadius, *r.chf) && rcBuildDistanceField(&ctx, *r.chf) &&
			                rcBuildRegions(&ctx, *r.chf, c.borderSize, c.minRegionArea, c.mergeRegionArea) &&
			                rcBuildContours(&ctx, *r.chf, c.maxSimplificationError, c.maxEdgeLen, *r.cset, RC_CONTOUR_TESS_WALL_EDGES | RC_CONTOUR_TESS_AREA_EDGES) &&
			                rcBuildPolyMesh(&ctx, *r.cset, c.maxVertsPerPoly, *r.pmesh);
			if (!ok)
				return error = std::format("square {},{}: {}", square.first, square.second, ctx.errors), false;

			const rcPolyMesh& pm = *r.pmesh;
			for (int i = 0; i < pm.npolys; ++i)
			{
				const unsigned short* p = &pm.polys[i * 2 * pm.nvp];
				Polygon poly;
				poly.surface = static_cast<Surface>(pm.areas[i]);
				for (int k = 0; k < pm.nvp && p[k] != RC_MESH_NULL_IDX; ++k)
				{
					const unsigned short* v = &pm.verts[p[k] * 3];
					// (x and y exactly on the voxel grid: the squares' sides come out the same on both sides)
					poly.v.push_back({pm.bmin[0] + v[0] * pm.cs, -(pm.bmin[2] + v[2] * pm.cs), pm.bmin[1] + v[1] * pm.ch});
				}
				if (poly.v.size() < 3)
					continue;
				float area = 0;
				for (size_t k = 0; k < poly.v.size(); ++k)
				{
					const Vec& a = poly.v[k];
					const Vec& b = poly.v[(k + 1) % poly.v.size()];
					area += a[0] * b[1] - b[0] * a[1];
				}
				if (area < 0)
					std::reverse(poly.v.begin(), poly.v.end());
				out.push_back(std::move(poly));
			}
			return true;
		}

		// Which side of its square an edge lies on: 0 west, 1 east, 2 south, 3 north, -1 none.
		int SideOf(const Square& sq, const Vec& a, const Vec& b)
		{
			const float x0 = SideX(sq.first, 0), x1 = SideX(sq.first, 1), y0 = SideX(sq.second, 0), y1 = SideX(sq.second, 1);
			if (a[0] == x0 && b[0] == x0)
				return 0;
			if (a[0] == x1 && b[0] == x1)
				return 1;
			if (a[1] == y0 && b[1] == y0)
				return 2;
			if (a[1] == y1 && b[1] == y1)
				return 3;
			return -1;
		}

		float SegmentDistance(float px, float py, const Vec& a, const Vec& b)
		{
			const float dx = b[0] - a[0], dy = b[1] - a[1], l = dx * dx + dy * dy;
			const float t = l > 0 ? std::clamp(((px - a[0]) * dx + (py - a[1]) * dy) / l, 0.0f, 1.0f) : 0.0f;
			return std::hypot(a[0] + t * dx - px, a[1] + t * dy - py);
		}

		// Distance along the ray (x, y) + t (dx, dy) to the segment, or infinity.
		float RayHit(float x, float y, float dx, float dy, const Vec& a, const Vec& b)
		{
			const float ex = b[0] - a[0], ey = b[1] - a[1], den = dx * ey - dy * ex;
			if (std::fabs(den) < 1e-9f)
				return std::numeric_limits<float>::infinity();
			const float t = ((a[0] - x) * ey - (a[1] - y) * ex) / den, u = ((a[0] - x) * dy - (a[1] - y) * dx) / den;
			return t > 0 && u >= 0 && u <= 1 ? t : std::numeric_limits<float>::infinity();
		}

		void PutVec(std::array<uint8_t, 16>& out, float x, float y, float z)
		{
			std::memcpy(out.data(), &x, 4);
			std::memcpy(out.data() + 4, &y, 4);
			std::memcpy(out.data() + 8, &z, 4);
			std::memcpy(out.data() + 12, &kTag, 4);
		}

		int16_t HalfMetres(float v) { return static_cast<int16_t>(std::clamp(std::trunc(v * 2.0f), -32768.0f, 32767.0f)); }

		// The quadtree: a square splits into four (NE, SE, SW, NW) while it has 32 polygons or more, down to 37.5 m; a
		// leaf lists the polygons whose box touches it. Heights as the game's files have them: the NE quarter takes the
		// upper half of its parent's, the SW quarter the lower half, the others none.
		int Node(ynv::Navmesh& m, const std::vector<std::array<float, 4>>& polyBoxes, float x0, float y0, float x1, float y1, float z0, float z1, int depth)
		{
			const int index = static_cast<int>(m.nodes.size());
			m.nodes.emplace_back();
			std::vector<uint16_t> inside;
			for (size_t i = 0; i < polyBoxes.size(); ++i)
			{
				const auto& b = polyBoxes[i];
				if (b[1] >= x0 && b[0] <= x1 && b[3] >= y0 && b[2] <= y1)
					inside.push_back(static_cast<uint16_t>(i));
			}
			{
				ynv::Node& n = m.nodes[index];
				PutVec(n.min, x0, y0, z0);
				PutVec(n.max, x1, y1, z1);
				n.box = {HalfMetres(x0), HalfMetres(x1), HalfMetres(y0), HalfMetres(y1), HalfMetres(z0), HalfMetres(z1)};
			}
			if (inside.size() < 32 || depth >= 2)
			{
				ynv::Node& n = m.nodes[index];
				n.leaf = true;
				n.polys = std::move(inside);
				return index;
			}
			const float xm = (x0 + x1) / 2, ym = (y0 + y1) / 2, zm = (z0 + z1) / 2;
			const int ne = Node(m, polyBoxes, xm, ym, x1, y1, zm, z1, depth + 1);
			const int se = Node(m, polyBoxes, xm, y0, x1, ym, 0, 0, depth + 1);
			const int sw = Node(m, polyBoxes, x0, y0, xm, ym, z0, zm, depth + 1);
			const int nw = Node(m, polyBoxes, x0, ym, xm, y1, 0, 0, depth + 1);
			m.nodes[index].children = {ne, se, sw, nw};
			return index;
		}
	}

	std::string FileName(const Square& square)
	{
		return std::format("navmesh[{}][{}].ynv", square.first * 3, square.second * 3);
	}

	bool Build(const bounds::Mesh& mesh, const std::vector<Surface>& surfaces, const Settings& settings, Result& out, std::string& error)
	{
		if (surfaces.size() != mesh.triangles.size())
			return error = "a surface per triangle is needed", false;
		// the triangles' boxes (x0 x1 y0 y1) and the squares the walkable ones reach into
		std::vector<std::array<float, 4>> boxes(mesh.triangles.size());
		std::vector<uint8_t> areas(mesh.triangles.size());
		std::set<Square> squares;
		std::map<Square, std::vector<uint32_t>> near; // the triangles reaching into each square or its border
		const float pad = (std::ceil(settings.agentRadius / settings.cellSize) + 3) * settings.cellSize;
		const auto squareOf = [](float v) { return static_cast<int>(std::floor((v - kOrigin) / kSquare)); };
		for (size_t t = 0; t < mesh.triangles.size(); ++t)
		{
			auto& b = boxes[t];
			b = {std::numeric_limits<float>::max(), -std::numeric_limits<float>::max(), std::numeric_limits<float>::max(), -std::numeric_limits<float>::max()};
			for (const uint32_t i : mesh.triangles[t])
			{
				const Vec& p = mesh.vertices[i];
				b = {std::min(b[0], p[0]), std::max(b[1], p[0]), std::min(b[2], p[1]), std::max(b[3], p[1])};
			}
			areas[t] = static_cast<uint8_t>(surfaces[t]);
			for (int sx = squareOf(b[0] - pad); sx <= squareOf(b[1] + pad); ++sx)
				for (int sy = squareOf(b[2] - pad); sy <= squareOf(b[3] + pad); ++sy)
				{
					if (sx < 0 || sy < 0 || sx >= settings.gridWidth)
						continue;
					near[{sx, sy}].push_back(static_cast<uint32_t>(t));
					// (a walkable triangle makes the squares it lies in, not the ones it only borders)
					if (areas[t] && sx >= squareOf(b[0]) && sx <= squareOf(b[1]) && sy >= squareOf(b[2]) && sy <= squareOf(b[3]))
						squares.insert({sx, sy});
				}
		}
		std::vector<SquareMesh> meshes;
		std::map<Square, int> slot;
		for (const Square& sq : squares)
		{
			SquareMesh sm{sq, {}};
			if (!BuildSquare(mesh, areas, boxes, near[sq], settings, sq, sm.polys, error))
				return false;
			if (sm.polys.empty())
				continue;
			slot[sq] = static_cast<int>(meshes.size());
			meshes.push_back(std::move(sm));
		}

		// each square's edges on a side cut at the vertices the square across has there (its original ones)
		struct SidePoint
		{
			float t, z;
		};
		std::vector<std::array<std::vector<SidePoint>, 4>> sidePoints(meshes.size());
		for (size_t m = 0; m < meshes.size(); ++m)
			for (const Polygon& p : meshes[m].polys)
				for (size_t k = 0; k < p.v.size(); ++k)
				{
					const Vec& a = p.v[k];
					const Vec& b = p.v[(k + 1) % p.v.size()];
					const int side = SideOf(meshes[m].square, a, b);
					if (side >= 0)
						for (const Vec* v : {&a, &b})
							sidePoints[m][side].push_back({side < 2 ? (*v)[1] : (*v)[0], (*v)[2]});
				}
		for (auto& sides : sidePoints)
			for (auto& points : sides)
				std::sort(points.begin(), points.end(), [](const SidePoint& a, const SidePoint& b) { return a.t < b.t; });
		static constexpr int kOpposite[4] = {1, 0, 3, 2};
		static constexpr int kStep[4][2] = {{-1, 0}, {1, 0}, {0, -1}, {0, 1}};
		for (size_t m = 0; m < meshes.size(); ++m)
		{
			const Square sq = meshes[m].square;
			for (Polygon& p : meshes[m].polys)
			{
				std::vector<Vec> v;
				for (size_t k = 0; k < p.v.size(); ++k)
				{
					const Vec a = p.v[k], b = p.v[(k + 1) % p.v.size()];
					v.push_back(a);
					const int side = SideOf(sq, a, b);
					if (side < 0)
						continue;
					const auto other = slot.find({sq.first + kStep[side][0], sq.second + kStep[side][1]});
					if (other == slot.end())
						continue;
					const auto& points = sidePoints[other->second][kOpposite[side]];
					const float ta = side < 2 ? a[1] : a[0], tb = side < 2 ? b[1] : b[0];
					std::vector<Vec> inserted;
					for (const SidePoint& sp : points)
					{
						if (sp.t <= std::min(ta, tb) || sp.t >= std::max(ta, tb))
							continue;
						const float f = (sp.t - ta) / (tb - ta), z = a[2] + f * (b[2] - a[2]);
						if (std::fabs(sp.z - z) > kSideTolerance)
							continue;
						const Vec q = side < 2 ? Vec{a[0], sp.t, sp.z} : Vec{sp.t, a[1], sp.z};
						if (!inserted.empty() && inserted.back()[0] == q[0] && inserted.back()[1] == q[1])
							continue;
						inserted.push_back(q);
					}
					if (ta > tb)
						std::reverse(inserted.begin(), inserted.end());
					out.cut += inserted.size();
					v.insert(v.end(), inserted.begin(), inserted.end());
				}
				p.v = std::move(v);
				p.across.assign(p.v.size(), {-1, -1});
			}
		}

		// edges joined: within a square by their vertices, across a side by position (and height)
		const auto key = [](const Vec& a, const Vec& b) { return std::array<float, 6>{a[0], a[1], a[2], b[0], b[1], b[2]}; };
		for (size_t m = 0; m < meshes.size(); ++m)
		{
			std::map<std::array<float, 6>, std::pair<int, int>> edges;
			auto& polys = meshes[m].polys;
			for (size_t i = 0; i < polys.size(); ++i)
				for (size_t k = 0; k < polys[i].v.size(); ++k)
					edges[key(polys[i].v[k], polys[i].v[(k + 1) % polys[i].v.size()])] = {static_cast<int>(i), static_cast<int>(k)};
			for (size_t i = 0; i < polys.size(); ++i)
				for (size_t k = 0; k < polys[i].v.size(); ++k)
				{
					const auto it = edges.find(key(polys[i].v[(k + 1) % polys[i].v.size()], polys[i].v[k]));
					if (it != edges.end())
						polys[i].across[k] = {static_cast<int>(m), it->second.first};
				}
		}
		std::map<std::array<float, 4>, std::vector<std::pair<int, int>>> sideEdges; // (x, y of both ends) -> (square, polygon)
		const auto sideKey = [](const Vec& a, const Vec& b) { return std::array<float, 4>{a[0], a[1], b[0], b[1]}; };
		for (size_t m = 0; m < meshes.size(); ++m)
			for (size_t i = 0; i < meshes[m].polys.size(); ++i)
			{
				const Polygon& p = meshes[m].polys[i];
				for (size_t k = 0; k < p.v.size(); ++k)
					if (SideOf(meshes[m].square, p.v[k], p.v[(k + 1) % p.v.size()]) >= 0)
						sideEdges[sideKey(p.v[k], p.v[(k + 1) % p.v.size()])].push_back({static_cast<int>(m), static_cast<int>(i)});
			}
		for (size_t m = 0; m < meshes.size(); ++m)
			for (Polygon& p : meshes[m].polys)
				for (size_t k = 0; k < p.v.size(); ++k)
				{
					const Vec& a = p.v[k];
					const Vec& b = p.v[(k + 1) % p.v.size()];
					if (SideOf(meshes[m].square, a, b) < 0)
						continue;
					const auto it = sideEdges.find(sideKey(b, a));
					if (it == sideEdges.end())
						continue;
					float best = kSideTolerance;
					for (const auto& [om, oi] : it->second)
					{
						if (om == static_cast<int>(m))
							continue;
						const Polygon& o = meshes[om].polys[oi];
						for (size_t j = 0; j < o.v.size(); ++j)
							if (o.v[j][0] == b[0] && o.v[j][1] == b[1] && o.v[(j + 1) % o.v.size()][0] == a[0] && o.v[(j + 1) % o.v.size()][1] == a[1])
							{
								const float dz = std::max(std::fabs(o.v[j][2] - b[2]), std::fabs(o.v[(j + 1) % o.v.size()][2] - a[2]));
								if (dz < best)
								{
									best = dz;
									p.across[k] = {om, oi};
								}
							}
					}
					if (p.across[k].first >= 0)
						++out.joined;
				}

		// the navmeshes
		for (size_t m = 0; m < meshes.size(); ++m)
		{
			const Square sq = meshes[m].square;
			const auto& polys = meshes[m].polys;
			if (polys.size() >= 0x7FFF)
				return error = std::format("square {},{}: {} polygons (at most 32766)", sq.first, sq.second, polys.size()), false;
			ynv::Navmesh nm;
			nm.index = static_cast<uint32_t>(sq.second * settings.gridWidth + sq.first);
			// the adjacency table: itself, none, then the neighbours there are (south, west, east, north)
			nm.adjacent = {nm.index, 0x3FFF};
			std::map<int, uint32_t> slotOf; // square slot -> adjacency slot
			slotOf[static_cast<int>(m)] = 0;
			for (const int side : {2, 0, 1, 3})
			{
				const auto other = slot.find({sq.first + kStep[side][0], sq.second + kStep[side][1]});
				if (other == slot.end())
					continue;
				slotOf[other->second] = static_cast<uint32_t>(nm.adjacent.size());
				nm.adjacent.push_back(static_cast<uint32_t>((sq.second + kStep[side][1]) * settings.gridWidth + sq.first + kStep[side][0]));
			}
			// vertices (u16 over the square's box)
			const float x0 = SideX(sq.first, 0), y0 = SideX(sq.second, 0);
			float zMin = std::numeric_limits<float>::max(), zMax = -std::numeric_limits<float>::max();
			for (const Polygon& p : polys)
				for (const Vec& v : p.v)
				{
					zMin = std::min(zMin, v[2]);
					zMax = std::max(zMax, v[2]);
				}
			const float zSize = zMax - zMin;
			std::map<std::array<uint16_t, 3>, uint16_t> vertexIndex;
			const auto quantize = [&](const Vec& v) {
				const auto q = [](float f) { return static_cast<uint16_t>(std::clamp(std::lround(f * 65535.0f), 0L, 65535L)); };
				return std::array<uint16_t, 3>{q((v[0] - x0) / kSquare), q((v[1] - y0) / kSquare), zSize > 0 ? q((v[2] - zMin) / zSize) : uint16_t(0)};
			};
			// boundary segments (edges with nothing across), for the free space and the directions in the way, filed in
			// cells of 4 m (a segment in every cell its box touches)
			std::vector<std::pair<Vec, Vec>> walls;
			for (const Polygon& p : polys)
				for (size_t k = 0; k < p.v.size(); ++k)
					if (p.across[k].first < 0)
						walls.push_back({p.v[k], p.v[(k + 1) % p.v.size()]});
			constexpr float kCell = 4.0f;
			constexpr int kCells = static_cast<int>(kSquare / kCell) + 1;
			std::vector<std::vector<uint32_t>> wallCells(kCells * kCells);
			const auto cellOf = [](float v, float origin) { return std::clamp(static_cast<int>((v - origin) / kCell), 0, kCells - 1); };
			for (size_t w = 0; w < walls.size(); ++w)
			{
				const auto& [wa, wb] = walls[w];
				for (int cx = cellOf(std::min(wa[0], wb[0]), x0); cx <= cellOf(std::max(wa[0], wb[0]), x0); ++cx)
					for (int cy = cellOf(std::min(wa[1], wb[1]), y0); cy <= cellOf(std::max(wa[1], wb[1]), y0); ++cy)
						wallCells[cy * kCells + cx].push_back(static_cast<uint32_t>(w));
			}
			// every wall filed within `reach` of (x, y), once each
			std::vector<uint32_t> visited(walls.size(), 0);
			uint32_t stamp = 0;
			const auto wallsNear = [&](float x, float y, float reach, const auto& visit) {
				++stamp;
				for (int cx = cellOf(x - reach, x0); cx <= cellOf(x + reach, x0); ++cx)
					for (int cy = cellOf(y - reach, y0); cy <= cellOf(y + reach, y0); ++cy)
						for (const uint32_t w : wallCells[cy * kCells + cx])
							if (visited[w] != stamp)
							{
								visited[w] = stamp;
								visit(walls[w].first, walls[w].second);
							}
			};
			std::vector<std::array<float, 4>> polyBoxes;
			for (size_t i = 0; i < polys.size(); ++i)
			{
				const Polygon& p = polys[i];
				ynv::Poly poly{};
				const size_t first = nm.indices.size();
				if (first + p.v.size() > 0xFFFF)
					return error = std::format("square {},{}: too many polygon corners (at most 65535)", sq.first, sq.second), false;
				std::array<float, 4> box = {std::numeric_limits<float>::max(), -std::numeric_limits<float>::max(), std::numeric_limits<float>::max(),
				                            -std::numeric_limits<float>::max()};
				float boxZ[2] = {std::numeric_limits<float>::max(), -std::numeric_limits<float>::max()};
				float cx = 0, cy = 0, cz = 0;
				for (size_t k = 0; k < p.v.size(); ++k)
				{
					const Vec& v = p.v[k];
					const auto q = quantize(v);
					auto it = vertexIndex.find(q);
					if (it == vertexIndex.end())
					{
						if (nm.vertices.size() >= 0xFFFF)
							return error = std::format("square {},{}: too many vertices", sq.first, sq.second), false;
						it = vertexIndex.emplace(q, static_cast<uint16_t>(nm.vertices.size())).first;
						nm.vertices.push_back({q[0], q[1], q[2]});
					}
					nm.indices.push_back(it->second);
					box = {std::min(box[0], v[0]), std::max(box[1], v[0]), std::min(box[2], v[1]), std::max(box[3], v[1])};
					boxZ[0] = std::min(boxZ[0], v[2]);
					boxZ[1] = std::max(boxZ[1], v[2]);
					cx += v[0] / p.v.size();
					cy += v[1] / p.v.size();
					cz += v[2] / p.v.size();
					// the edge: the polygon across and its slot, the free space beside it (0.25 m units)
					ynv::Edge e{kNone, kNone};
					if (p.across[k].first >= 0)
					{
						const uint32_t across = (static_cast<uint32_t>(p.across[k].second) << 5) | slotOf.at(p.across[k].first);
						const Vec& b = p.v[(k + 1) % p.v.size()];
						float free = 31 * 0.25f;
						wallsNear((v[0] + b[0]) / 2, (v[1] + b[1]) / 2, free, [&](const Vec& wa, const Vec& wb) {
							if (std::fabs((wa[2] + wb[2]) / 2 - (v[2] + b[2]) / 2) < 2.5f)
								free = std::min(free, SegmentDistance((v[0] + b[0]) / 2, (v[1] + b[1]) / 2, wa, wb));
						});
						e.a = across | static_cast<uint32_t>(std::min(31, static_cast<int>(free / 0.25f))) << 22;
						e.b = across;
					}
					nm.edges.push_back(e);
				}
				polyBoxes.push_back(box);
				const uint16_t flags = p.surface == Surface::Pavement ? 1 : p.surface == Surface::Road ? 2 : 0;
				std::memcpy(poly.raw + 0x00, &flags, 2);
				poly.SetVertexCount(static_cast<uint32_t>(p.v.size()));
				poly.SetFirstIndex(static_cast<uint16_t>(first));
				poly.SetNavmesh(static_cast<uint16_t>(nm.index));
				const int16_t halves[6] = {HalfMetres(box[0]), HalfMetres(box[1]), HalfMetres(box[2]), HalfMetres(box[3]), HalfMetres(boxZ[0]), HalfMetres(boxZ[1])};
				std::memcpy(poly.raw + 0x18, halves, sizeof(halves));
				const uint32_t flags2 = p.surface == Surface::Road ? 0x420 : 0;
				std::memcpy(poly.raw + 0x24, &flags2, 4);
				for (int a = 0; a < 2; ++a)
				{
					const float b0 = halves[2 * a] * 0.5f, b1 = halves[2 * a + 1] * 0.5f, c = a ? cy : cx;
					poly.raw[0x28 + a] = b1 > b0 ? static_cast<uint8_t>(std::clamp(std::lround((c - b0) / (b1 - b0) * 255.0f), 0L, 255L)) : 0;
				}
				// the directions with something in the way within 8 m: bit 0 north, then anticlockwise
				uint8_t around = 0;
				wallsNear(cx, cy, 8.0f, [&](const Vec& wa, const Vec& wb) {
					if (std::fabs((wa[2] + wb[2]) / 2 - cz) >= 3.0f)
						return;
					for (int d = 0; d < 8; ++d)
					{
						const float angle = 3.14159265f / 2 + d * 3.14159265f / 4;
						if (!(around >> d & 1) && RayHit(cx, cy, std::cos(angle), std::sin(angle), wa, wb) < 8.0f)
							around |= static_cast<uint8_t>(1 << d);
					}
				});
				poly.raw[0x2A] = around;
				const uint32_t chunk = static_cast<uint32_t>(i / kPolysPerChunk) << 4;
				std::memcpy(poly.raw + 0x2C, &chunk, 4);
				nm.polys.push_back(poly);
			}
			// root: flags, the format, an identity matrix and the size, as the game's files have them
			nm.flags = 1;
			nm.format = 0x10011;
			for (int r = 0; r < 4; ++r)
			{
				std::array<uint8_t, 16> row{};
				PutVec(row, r == 0 ? 1.0f : 0.0f, r == 1 ? 1.0f : 0.0f, r == 2 ? 1.0f : 0.0f);
				std::memcpy(nm.matrix.data() + 16 * r, row.data(), 16);
			}
			PutVec(nm.size, kSquare, kSquare, zSize);
			Node(nm, polyBoxes, x0, y0, x0 + kSquare, y0 + kSquare, zMin, zMax, 0);
			out.polygons += polys.size();
			out.navmeshes[sq] = std::move(nm);
		}
		return true;
	}
}
