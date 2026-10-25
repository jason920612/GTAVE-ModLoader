#include "build.hpp"

#include <algorithm>
#include <cmath>
#include <format>
#include <fstream>
#include <limits>
#include <map>
#include <set>

#include "bounds.hpp"
#include "buildings.hpp"
#include "convert/pso.hpp"
#include "mapfiles.hpp"
#include "navgen.hpp"
#include "paths.hpp"
#include "raster.hpp"
#include "roads.hpp"
#include "terrain.hpp"
#include "ybn.hpp"
#include "ydr.hpp"
#include "ynv.hpp"

namespace worldgen::build
{
	namespace
	{
		using loader::convert::Bytes;
		using P2 = std::array<float, 2>;
		using P3 = std::array<float, 3>;

		// ground classes (the land grid)
		enum : uint8_t { kWater, kLand, kBeach, kUrban, kPark, kPaved };
		// materials.dat rows
		constexpr uint8_t kGrass = 47, kSand = 19, kConcrete = 1;

		float Smooth(float t)
		{
			t = std::clamp(t, 0.0f, 1.0f);
			return t * t * (3 - 2 * t);
		}

		// Gentle rolling ground (a few metres' worth of waves at several scales), deterministic.
		float Noise(float x, float y)
		{
			return 0.5f * std::sin(x * 0.0071f + 1.3f) * std::cos(y * 0.0063f + 0.4f) + 0.3f * std::sin(x * 0.019f - y * 0.013f + 2.1f) +
			       0.2f * std::cos(x * 0.041f + y * 0.037f);
		}

		bool WriteFile(const std::filesystem::path& p, const Bytes& b)
		{
			std::ofstream o(p, std::ios::binary);
			o.write(reinterpret_cast<const char*>(b.data()), static_cast<std::streamsize>(b.size()));
			return static_cast<bool>(o);
		}

		bool GameMaterial(const ue5export::GameFiles& files, const char* model, uint32_t index, ydr::Material& out, std::string& error)
		{
			Bytes data;
			loader::convert::Resource res;
			const auto* f = files.Find(model);
			if (!f || !files.Read(*f, data, error) || !loader::convert::Read(data, res, error))
				return error = std::string("template model ") + model + ": " + error, false;
			return ydr::ReadMaterial(ue5export::drawable::View(res), ue5export::drawable::View::kRoot, index, out, error);
		}

		// A mesh's triangles in one tile (by their centres), moved to around (cx, cy).
		roads::Mesh Clip(const roads::Mesh& m, float x0, float y0, float x1, float y1, float cx, float cy)
		{
			roads::Mesh out;
			std::map<uint32_t, uint32_t> remap;
			for (size_t t = 0; t + 2 < m.indices.size(); t += 3)
			{
				const auto& a = m.vertices[m.indices[t]].position;
				const auto& b = m.vertices[m.indices[t + 1]].position;
				const auto& c = m.vertices[m.indices[t + 2]].position;
				const float mx = (a[0] + b[0] + c[0]) / 3, my = (a[1] + b[1] + c[1]) / 3;
				if (mx < x0 || mx >= x1 || my < y0 || my >= y1)
					continue;
				for (int k = 0; k < 3; ++k)
				{
					const uint32_t i = m.indices[t + k];
					auto it = remap.find(i);
					if (it == remap.end())
					{
						ydr::Vertex v = m.vertices[i];
						v.position[0] -= cx, v.position[1] -= cy;
						out.vertices.push_back(v);
						it = remap.emplace(i, static_cast<uint32_t>(out.vertices.size() - 1)).first;
					}
					out.indices.push_back(it->second);
				}
			}
			return out;
		}

		// A mesh split into meshes of at most 65535 vertices.
		void AddMeshes(ydr::Drawable& d, uint32_t material, const roads::Mesh& m)
		{
			ydr::Mesh cur;
			cur.material = material;
			std::map<uint32_t, uint32_t> remap;
			for (size_t t = 0; t + 2 < m.indices.size(); t += 3)
			{
				if (cur.vertices.size() + 3 > 0xFFFF)
				{
					d.lods[0].push_back(std::move(cur));
					cur = {};
					cur.material = material;
					remap.clear();
				}
				for (int k = 0; k < 3; ++k)
				{
					const uint32_t i = m.indices[t + k];
					auto it = remap.find(i);
					if (it == remap.end())
					{
						cur.vertices.push_back(m.vertices[i]);
						it = remap.emplace(i, static_cast<uint32_t>(cur.vertices.size() - 1)).first;
					}
					cur.indices.push_back(it->second);
				}
			}
			if (!cur.indices.empty())
				d.lods[0].push_back(std::move(cur));
		}
	}

	bool Run(const ue5export::GameFiles& files, const world::World& w, const std::vector<catalog::Item>& catalog, const Options& o, Stats& stats,
	    std::string& error)
	{
		const bool partial = o.area[2] > o.area[0];
		const float bx0 = partial ? o.area[0] : w.min[0], by0 = partial ? o.area[1] : w.min[1], bx1 = partial ? o.area[2] : w.max[0],
		            by1 = partial ? o.area[3] : w.max[1];
		// the grids reach a little past the work box (distances, roads near its edges)
		const float margin = 400;
		const float gx0 = std::max(w.min[0], bx0 - margin), gy0 = std::max(w.min[1], by0 - margin), gx1 = std::min(w.max[0], bx1 + margin),
		            gy1 = std::min(w.max[1], by1 + margin);

		// 1. land and water, the ground's classes
		raster::Grid<uint8_t> land;
		land.Init(gx0, gy0, gx1, gy1, o.classify, kWater);
		raster::ClassifyCoast(land, w.coastlines, 200.0f);
		const auto fill = [&](const char* kind, uint8_t value) {
			for (const auto& a : w.areas)
				if (a.kind == kind)
				{
					auto rings = a.outer;
					rings.insert(rings.end(), a.inner.begin(), a.inner.end());
					// (land use only on land; water anywhere)
					raster::ForPolygon(land, rings, [&](int i, int j) {
						if (land.At(i, j) != kWater || value == kWater)
							land.At(i, j) = value;
					});
				}
		};
		for (const char* k : {"residential", "commercial", "retail", "industrial", "construction", "railway", "military"})
			fill(k, kUrban);
		for (const char* k : {"park", "grass", "pitch", "wood", "wetland"})
			fill(k, kPark);
		fill("parking", kPaved);
		fill("paved", kPaved);
		fill("beach", kBeach);
		fill("water", kWater);
		for (const auto& ww : w.waterways)
			raster::DrawLine(land, ww.points, ww.width / 2, uint8_t{kWater});
		// the island: no land within `border` of the box's edges
		for (int j = 0; j < land.ny; ++j)
			for (int i = 0; i < land.nx; ++i)
			{
				const P2 c = land.Centre(i, j);
				const float edge = std::min({c[0] - w.min[0], w.max[0] - c[0], c[1] - w.min[1], w.max[1] - c[1]});
				// (a little wavy, so the coast there is not a ruler line)
				if (edge < o.border + 60 * Noise(c[0] * 3, c[1] * 3))
					land.At(i, j) = kWater;
			}
		// (a picture of the classes, for checking: water, land, beach, built-up, park, paved)
		{
			const std::array<uint8_t, 3> colours[] = {{40, 90, 160}, {150, 190, 110}, {235, 220, 160}, {180, 170, 160}, {90, 150, 70}, {120, 120, 120}};
			std::vector<std::array<uint8_t, 3>> rgb(land.v.size());
			for (size_t k = 0; k < land.v.size(); ++k)
				rgb[k] = colours[std::min<uint8_t>(land.v[k], 5)];
			std::filesystem::create_directories(o.out);
			raster::WritePng(o.out.parent_path() / (o.name + "_land.png"), land.nx, land.ny, rgb, error);
		}
		// 2. heights: land rises gently from the shore (a beach lower), the sea floor deepens away from it
		// (DistanceTo measures from the cells of a value to the nearest other cell)
		raster::Grid<uint8_t> dry = land, wet = land;
		for (size_t k = 0; k < land.v.size(); ++k)
			dry.v[k] = land.v[k] != kWater, wet.v[k] = land.v[k] == kWater;
		const auto toWater = raster::DistanceTo(dry, 1); // land cells: how far the water is
		const auto toLand = raster::DistanceTo(wet, 1);  // water cells: how far the land is
		raster::Grid<float> height;
		height.x0 = land.x0, height.y0 = land.y0, height.cell = land.cell, height.nx = land.nx, height.ny = land.ny;
		height.v.resize(land.v.size());
		for (int j = 0; j < land.ny; ++j)
			for (int i = 0; i < land.nx; ++i)
			{
				const P2 c = land.Centre(i, j);
				const uint8_t k = land.At(i, j);
				float h;
				if (k != kWater)
				{
					const float d = toWater.At(i, j);
					// (high enough that the sea's waves stay off it but on beaches, which they wash)
					h = 1.4f + 2.2f * Smooth(d / 150) + 0.4f * Noise(c[0], c[1]) * Smooth(d / 150);
					if (k == kBeach)
						h = std::min(h, 0.6f + 1.6f * Smooth(d / 60));
				}
				else
				{
					const float d = toLand.At(i, j);
					h = -(0.7f + 3.3f * (1 - std::exp(-d / 70)) + 22.0f * (1 - std::exp(-std::max(0.0f, d - 300) / 1500)));
				}
				height.At(i, j) = h;
			}
		for (int pass = 0; pass < 2; ++pass)
		{
			auto copy = height.v;
			for (int j = 1; j + 1 < height.ny; ++j)
				for (int i = 1; i + 1 < height.nx; ++i)
				{
					float s = 0;
					for (int dj = -1; dj <= 1; ++dj)
						for (int di = -1; di <= 1; ++di)
							s += copy[static_cast<size_t>(j + dj) * height.nx + (i + di)];
					height.At(i, j) = s / 9;
				}
		}
		const auto ground = [&](float x, float y) {
			const float fx = std::clamp((x - height.x0) / height.cell - 0.5f, 0.0f, height.nx - 1.001f),
			            fy = std::clamp((y - height.y0) / height.cell - 0.5f, 0.0f, height.ny - 1.001f);
			const int i = static_cast<int>(fx), j = static_cast<int>(fy);
			const float tx = fx - i, ty = fy - j;
			const float a = height.At(i, j), b = height.At(i + 1, j), c = height.At(i, j + 1), d = height.At(i + 1, j + 1);
			return (a * (1 - tx) + b * tx) * (1 - ty) + (c * (1 - tx) + d * tx) * ty;
		};

		// 3. roads (those reaching into the grids)
		world::World near = w;
		std::erase_if(near.roads, [&](const world::Road& r) {
			return std::none_of(r.points.begin(), r.points.end(), [&](const P2& p) { return p[0] >= gx0 && p[0] <= gx1 && p[1] >= gy0 && p[1] <= gy1; });
		});
		roads::Network network = roads::Build(near);
		roads::Heights(network, ground);
		roads::Surfaces surfaces;
		roads::Geometry(network, surfaces);
		stats.roadEdges = network.edges.size();
		// the ground under them: lowered below their surfaces (a grid of the lowest road surface over each cell)
		raster::Grid<float> floor;
		floor.x0 = land.x0, floor.y0 = land.y0, floor.cell = land.cell, floor.nx = land.nx, floor.ny = land.ny;
		floor.v.assign(land.v.size(), std::numeric_limits<float>::max());
		for (const auto& t : surfaces.footprint)
		{
			const float minX = std::min({t[0][0], t[1][0], t[2][0]}), maxX = std::max({t[0][0], t[1][0], t[2][0]});
			const float minY = std::min({t[0][1], t[1][1], t[2][1]}), maxY = std::max({t[0][1], t[1][1], t[2][1]});
			const auto c0 = floor.Cell({minX, minY}), c1 = floor.Cell({maxX, maxY});
			const float z = std::min({t[0][2], t[1][2], t[2][2]});
			for (int j = std::max(0, c0[1] - 1); j <= std::min(floor.ny - 1, c1[1] + 1); ++j)
				for (int i = std::max(0, c0[0] - 1); i <= std::min(floor.nx - 1, c1[0] + 1); ++i)
					floor.At(i, j) = std::min(floor.At(i, j), z);
		}

		// buildings: the game's models on the footprints (the ground under each no higher than its floor)
		std::vector<buildings::Placement> placed;
		size_t unmatched = 0;
		if (o.buildings && !catalog.empty())
		{
			const buildings::Matcher matcher(catalog);
			for (const auto& b : w.buildings)
			{
				P2 c{0, 0};
				for (const P2& q : b.footprint)
					c[0] += q[0] / b.footprint.size(), c[1] += q[1] / b.footprint.size();
				if (c[0] < bx0 || c[0] >= bx1 || c[1] < by0 || c[1] >= by1)
					continue;
				float base = ground(c[0], c[1]);
				for (const P2& q : b.footprint)
					base = std::min(base, ground(q[0], q[1]));
				if (base < 0.3f)
					continue; // (in the water)
				buildings::Placement p;
				if (!matcher.Pick(b.footprint, b.height, base, b.kind, p))
				{
					++unmatched;
					continue;
				}
				placed.push_back(p);
				raster::ForPolygon(floor, {b.footprint}, [&](int i, int j) { floor.At(i, j) = std::min(floor.At(i, j), base + 0.15f); });
			}
			stats.buildings = placed.size();
			std::printf("buildings: %zu placed, %zu without a model that fits (%zu models to choose from)\n", placed.size(), unmatched, matcher.Candidates());
		}

		// 4. tiles: terrain and roads, their collision; entities and archetypes
		ydr::Material ground3, asphalt, sidewalk, kerb, marking;
		// (terrain_cb_4lyr: the terrain shader without the rain's puddles, which on flat ground mirror the sky in blue patches)
		if (!GameMaterial(files, "kt1_04_02_r3iint.ydr", 4, ground3, error) || !GameMaterial(files, "dt1_rd1_r1_05.ydr", 1, asphalt, error) ||
		    !GameMaterial(files, "dt1_rd1_r1_05.ydr", 3, sidewalk, error) || !GameMaterial(files, "dt1_rd1_r1_05.ydr", 0, kerb, error) ||
		    !GameMaterial(files, "dt1_rd1_r1_05.ydr", 5, marking, error))
			return false;
		{
			const std::pair<const char*, const char*> layers[] = {{"IM_Grass02V", "IM_Grass02_N"},
			    {"RSN_OS_beachhard", "NXG_RSN_OS_beachhard_n"},
			    {"NXG_RSN_OS_beachsoftprints", "NXG_RSN_OS_beachsoftprints_n"},
			    {"RSN_OS_beachhardwater", "RSN_OS_beachhardwater_n"}};
			for (int k = 0; k < 4; ++k)
			{
				ground3.Texture(loader::convert::Joaat(std::format("DiffuseTexture_layer{}", k)), layers[k].first);
				ground3.Texture(loader::convert::Joaat(std::format("BumpTexture_layer{}", k)), layers[k].second);
			}
		}
		mapfiles::Archetype beachTypes, roadTypes;
		if (!mapfiles::FindArchetype(files, "vb_35_beacha", beachTypes, error) || !mapfiles::FindArchetype(files, "dt1_rd1_r1_05", roadTypes, error))
			return false;
		std::filesystem::create_directories(o.out);
		const int per = std::max(1, static_cast<int>(std::lround(o.tile / o.cell)));
		const int ti0 = static_cast<int>(std::floor((bx0 - w.min[0]) / o.tile)), tj0 = static_cast<int>(std::floor((by0 - w.min[1]) / o.tile));
		const int ti1 = static_cast<int>(std::ceil((bx1 - w.min[0]) / o.tile)), tj1 = static_cast<int>(std::ceil((by1 - w.min[1]) / o.tile));
		std::vector<mapfiles::Archetype> archetypes;
		std::map<std::pair<int, int>, std::vector<mapfiles::Entity>> blocks; // 1 km blocks of entities
		bounds::Mesh navMesh;
		std::vector<navgen::Surface> navSurfaces;
		for (int tj = tj0; tj < tj1; ++tj)
			for (int ti = ti0; ti < ti1; ++ti)
			{
				const float x0 = w.min[0] + ti * o.tile, y0 = w.min[1] + tj * o.tile, x1 = x0 + o.tile, y1 = y0 + o.tile;
				const float cx = x0 + o.tile / 2, cy = y0 + o.tile / 2;
				// the terrain field of this tile (vertices every `cell` m)
				terrain::Field f;
				f.x0 = x0, f.y0 = y0, f.cell = o.cell, f.nx = f.ny = per;
				bool anyLand = false;
				for (int j = 0; j <= per; ++j)
					for (int i = 0; i <= per; ++i)
					{
						const float x = x0 + i * o.cell, y = y0 + j * o.cell;
						float h = ground(x, y);
						const auto c = floor.Cell({x, y});
						if (floor.In(c[0], c[1]) && floor.At(c[0], c[1]) < std::numeric_limits<float>::max())
							h = std::min(h, floor.At(c[0], c[1]) - 0.15f);
						f.height.push_back(h);
						const auto lc = land.Cell({x, y});
						const uint8_t k = land.In(lc[0], lc[1]) ? land.At(lc[0], lc[1]) : kWater;
						anyLand |= k != kWater || h > -3;
						const float d = land.In(lc[0], lc[1]) ? toWater.At(lc[0], lc[1]) : 0;
						// layers: grass, hard sand (built-up ground), soft sand (beach, the shore), wet sand (under water)
						float grass = 0, hard = 0, soft = 0, wetSand = 0;
						if (k == kWater || h < 0.2f)
							wetSand = 1;
						else if (k == kBeach)
							soft = 1;
						else if (k == kPaved)
							hard = 0.7f, grass = 0.3f;
						else if (k == kUrban)
							hard = 0.25f, grass = 0.75f;
						else
							grass = 1;
						if (k != kWater && k != kBeach && d < 14)
						{
							const float t = Smooth(d / 14);
							grass *= t, hard *= t, soft = soft * t + (1 - t);
						}
						f.layers.push_back(terrain::Layers(grass, hard, soft, wetSand));
					}
				for (int j = 0; j < per; ++j)
					for (int i = 0; i < per; ++i)
					{
						const auto lc = land.Cell({x0 + (i + 0.5f) * o.cell, y0 + (j + 0.5f) * o.cell});
						const uint8_t k = land.In(lc[0], lc[1]) ? land.At(lc[0], lc[1]) : kWater;
						f.material.push_back(k == kWater || k == kBeach ? kSand : k == kPaved ? kConcrete : kGrass);
					}
				if (!anyLand)
					continue; // (deep open sea: nothing to see or walk on)
				const std::string tname = std::format("{}_t_{}_{}", o.name, ti, tj), rname = std::format("{}_r_{}_{}", o.name, ti, tj),
				                  cname = std::format("{}_c_{}_{}", o.name, ti, tj);
				ydr::Drawable td;
				td.name = tname;
				td.materials.push_back(ground3);
				td.lods[0].push_back(terrain::TileMesh(f, 0, 0, per, cx, cy, 5.0f));
				stats.terrainTriangles += td.lods[0][0].indices.size() / 3;
				Bytes file;
				if (!ydr::Write(td, file, error))
					return error = tname + ": " + error, false;
				WriteFile(o.out / (tname + ".ydr"), file);
				mapfiles::Archetype ta;
				ta.name = tname;
				ta.textureDictionary = beachTypes.textureDictionary;
				ta.lodDist = 2500;
				ydr::Box(td, ta.min, ta.max);
				archetypes.push_back(ta);
				const int bi = static_cast<int>(std::floor((cx - w.min[0]) / 1000)), bj = static_cast<int>(std::floor((cy - w.min[1]) / 1000));
				mapfiles::Entity te;
				te.archetype = loader::convert::Joaat(tname);
				te.position = {cx, cy, 0};
				te.lodDist = 2500;
				te.radius = o.tile * 0.75f;
				blocks[{bi, bj}].push_back(te);
				// collision: the terrain, and the roads' triangles centred in this tile
				auto parts = terrain::TileCollision(f, 0, 0, per);
				ybn::Part part;
				std::map<uint8_t, uint8_t> materialAt;
				const auto flush = [&] {
					if (!part.triangles.empty())
						parts.push_back(std::move(part));
					part = {};
					materialAt.clear();
				};
				const auto& col = surfaces.collision;
				for (size_t t = 0; t < col.triangles.size(); ++t)
				{
					const auto& tri = col.triangles[t];
					const P3& a = col.vertices[tri[0]];
					const P3& b = col.vertices[tri[1]];
					const P3& c = col.vertices[tri[2]];
					const float mx = (a[0] + b[0] + c[0]) / 3, my = (a[1] + b[1] + c[1]) / 3;
					if (mx < x0 || mx >= x1 || my < y0 || my >= y1)
						continue;
					if (part.triangles.size() >= 1000 || part.vertices.size() + 3 > 2730)
						flush();
					auto mat = materialAt.find(col.materials[t]);
					if (mat == materialAt.end())
					{
						mat = materialAt.emplace(col.materials[t], static_cast<uint8_t>(part.materials.size())).first;
						part.materials.push_back(ybn::Material(col.materials[t]));
					}
					const uint32_t base = static_cast<uint32_t>(part.vertices.size());
					part.vertices.insert(part.vertices.end(), {a, b, c});
					part.triangles.push_back({base, base + 1, base + 2});
					part.triangleMaterials.push_back(mat->second);
					stats.roadTriangles += 1;
				}
				flush();
				// the buildings standing in this tile: entities, their boxes (obstacles for the navmesh)
				std::vector<P3> boxVertices;
				std::vector<std::array<uint32_t, 3>> boxTriangles;
				for (const auto& p : placed)
				{
					if (p.centre[0] < x0 || p.centre[0] >= x1 || p.centre[1] < y0 || p.centre[1] >= y1)
						continue;
					mapfiles::Entity be;
					be.archetype = p.item->hash;
					be.position = p.position;
					be.rotation = {0, 0, std::sin(p.heading / 2), std::cos(p.heading / 2)};
					be.lodDist = p.lodDist;
					be.radius = std::hypot(p.item->max[0] - p.item->min[0], p.item->max[1] - p.item->min[1], p.item->max[2] - p.item->min[2]) / 2;
					blocks[{bi, bj}].push_back(be);
					if (part.triangles.size() + 12 > 1000 || part.vertices.size() + 8 > 2730)
						flush();
					const size_t before = part.vertices.size();
					std::vector<std::array<uint32_t, 3>> tris;
					buildings::CollisionBox(p, part.vertices, tris);
					auto mat = materialAt.find(kConcrete);
					if (mat == materialAt.end())
					{
						mat = materialAt.emplace(kConcrete, static_cast<uint8_t>(part.materials.size())).first;
						part.materials.push_back(ybn::Material(kConcrete));
					}
					for (const auto& t : tris)
						part.triangles.push_back(t), part.triangleMaterials.push_back(mat->second);
					const uint32_t navBase = static_cast<uint32_t>(boxVertices.size());
					boxVertices.insert(boxVertices.end(), part.vertices.begin() + static_cast<ptrdiff_t>(before), part.vertices.end());
					for (const auto& t : tris)
						boxTriangles.push_back({navBase + t[0] - static_cast<uint32_t>(before), navBase + t[1] - static_cast<uint32_t>(before),
						    navBase + t[2] - static_cast<uint32_t>(before)});
				}
				flush();
				// (a tile's collision: at most 256 parts)
				if (parts.size() > 256)
					parts.resize(256);
				Bytes collision;
				if (!ybn::Write(parts, collision, error))
					return error = cname + ": " + error, false;
				WriteFile(o.out / (cname + ".ybn"), collision);
				// for the navmesh: the same triangles (the buildings' boxes in the way, never walked on)
				{
					const uint32_t base = static_cast<uint32_t>(navMesh.vertices.size());
					navMesh.vertices.insert(navMesh.vertices.end(), boxVertices.begin(), boxVertices.end());
					for (const auto& t : boxTriangles)
					{
						navMesh.triangles.push_back({base + t[0], base + t[1], base + t[2]});
						navMesh.materials.push_back(kConcrete);
						navSurfaces.push_back(navgen::Surface::None);
					}
				}
				std::set<std::array<P3, 3>> boxSet;
				for (const auto& t : boxTriangles)
					boxSet.insert({boxVertices[t[0]], boxVertices[t[1]], boxVertices[t[2]]});
				for (const auto& p : parts)
				{
					const uint32_t base = static_cast<uint32_t>(navMesh.vertices.size());
					navMesh.vertices.insert(navMesh.vertices.end(), p.vertices.begin(), p.vertices.end());
					for (size_t t = 0; t < p.triangles.size(); ++t)
					{
						if (boxSet.contains({p.vertices[p.triangles[t][0]], p.vertices[p.triangles[t][1]], p.vertices[p.triangles[t][2]]}))
							continue; // (a building's: added above)
						navMesh.triangles.push_back({base + p.triangles[t][0], base + p.triangles[t][1], base + p.triangles[t][2]});
						const uint8_t m = p.materials[p.triangleMaterials[t]][0];
						navMesh.materials.push_back(m);
						navSurfaces.push_back(m >= 4 && m <= 6 ? navgen::Surface::Road : m == 1 || m == 15 || m == 174 ? navgen::Surface::Pavement
						                                                                                                : navgen::Surface::Ground);
					}
				}
				// the roads' model
				ydr::Drawable rd;
				rd.name = rname;
				rd.materials = {asphalt, sidewalk, kerb, marking};
				AddMeshes(rd, 0, Clip(surfaces.asphalt, x0, y0, x1, y1, cx, cy));
				AddMeshes(rd, 1, Clip(surfaces.sidewalk, x0, y0, x1, y1, cx, cy));
				AddMeshes(rd, 2, Clip(surfaces.kerb, x0, y0, x1, y1, cx, cy));
				AddMeshes(rd, 3, Clip(surfaces.markings, x0, y0, x1, y1, cx, cy));
				if (!rd.lods[0].empty())
				{
					if (!ydr::Write(rd, file, error))
						return error = rname + ": " + error, false;
					WriteFile(o.out / (rname + ".ydr"), file);
					mapfiles::Archetype ra;
					ra.name = rname;
					ra.textureDictionary = roadTypes.textureDictionary;
					ra.lodDist = 1200;
					ydr::Box(rd, ra.min, ra.max);
					archetypes.push_back(ra);
					mapfiles::Entity re;
					re.archetype = loader::convert::Joaat(rname);
					re.position = {cx, cy, 0};
					re.lodDist = 1200;
					re.radius = o.tile * 0.75f;
					blocks[{bi, bj}].push_back(re);
				}
				++stats.tiles;
			}
		// 5. archetypes and maps. The game's models placed here are defined in archetype files the game loads only with
		// their own areas' maps: the pack asks for those to be kept loaded (requests.txt: their paths, for worldgen pack's
		// DLC_ITYP_REQUEST entries; under a new name a model is not found: the game looks its model up by the
		// archetype's name).
		{
			std::set<std::string> paths;
			for (const auto& p : placed)
			{
				const auto* f = files.Find(p.item->ytyp);
				if (!f)
					continue;
				const size_t at = f->path.find("levels/");
				if (at != std::string::npos)
					paths.insert("platform:/" + f->path.substr(at));
			}
			std::ofstream requests(o.out / "requests.txt");
			for (const auto& path : paths)
				requests << path << "\n";
			std::printf("buildings: %zu archetype files of the game kept loaded\n", paths.size());
		}
		Bytes types;
		if (!mapfiles::WriteTypes(files, o.name, archetypes, types, error))
			return false;
		WriteFile(o.out / (o.name + ".ytyp"), types);
		for (const auto& [block, entities] : blocks)
		{
			Bytes map;
			const std::string mname = std::format("{}_{}_{}", o.name, block.first, block.second);
			if (!mapfiles::WriteMap(files, mname, entities, map, error))
				return false;
			WriteFile(o.out / (mname + ".ymap"), map);
		}
		// 6. pedestrians' navmeshes (the sea floor half a metre down and more left out)
		if (o.navmesh && !navMesh.triangles.empty())
		{
			for (size_t t = 0; t < navMesh.triangles.size(); ++t)
				if (std::all_of(navMesh.triangles[t].begin(), navMesh.triangles[t].end(), [&](uint32_t v) { return navMesh.vertices[v][2] < -0.5f; }))
					navSurfaces[t] = navgen::Surface::None;
			navgen::Result result;
			if (!navgen::Build(navMesh, navSurfaces, {}, result, error))
				return false;
			for (const auto& [square, nm] : result.navmeshes)
			{
				Bytes file;
				if (!ynv::Write(nm, file, error))
					return error = navgen::FileName(square) + ": " + error, false;
				WriteFile(o.out / navgen::FileName(square), file);
			}
			stats.navmeshes = result.navmeshes.size();
		}
		// 7. traffic: the road graph's nodes, written with the game's regions
		if (o.paths)
		{
			std::vector<roads::PathNode> nodes;
			std::vector<roads::PathLink> links;
			roads::Paths(network, 20.0f, nodes, links);
			// (only the nodes in the work box and the links between them, for a part)
			if (partial)
			{
				std::vector<int> keep(nodes.size(), -1);
				std::vector<roads::PathNode> kept;
				for (size_t i = 0; i < nodes.size(); ++i)
					if (nodes[i].p[0] >= bx0 && nodes[i].p[0] <= bx1 && nodes[i].p[1] >= by0 && nodes[i].p[1] <= by1)
						keep[i] = static_cast<int>(kept.size()), kept.push_back(nodes[i]);
				std::vector<roads::PathLink> keptLinks;
				for (const auto& l : links)
					if (keep[l.a] >= 0 && keep[l.b] >= 0)
						keptLinks.push_back({keep[l.a], keep[l.b], l.lanesAB, l.lanesBA, l.width});
				nodes = std::move(kept), links = std::move(keptLinks);
			}
			paths::Stats ps;
			if (!paths::Write(files, {w.min[0], w.min[1], w.max[0], w.max[1]}, nodes, links, o.out, ps, error))
				return false;
			stats.pathNodes = ps.newNodes, stats.regions = ps.regions;
		}
		// 8. population zones: 1 km blocks of land, bound like a game zone of their kind (beach, built-up, the rest)
		if (!o.zonesOut.empty())
		{
			Bytes data;
			const auto* zf = files.Find("zonebind.ymt");
			if (!zf || !files.Read(*zf, data, error))
				return error = "zonebind.ymt: " + error, false;
			loader::convert::pso::Entries game, ours;
			if (!game.Read(std::string(data.begin(), data.end()), error) || !ours.Read(std::string(data.begin(), data.end()), error))
				return false;
			ours.Clear();
			std::string ipl = "zone\n";
			for (float y = std::floor(by0 / 1000) * 1000; y < by1; y += 1000)
				for (float x = std::floor(bx0 / 1000) * 1000; x < bx1; x += 1000)
				{
					size_t landCells = 0, beach = 0, urban = 0;
					for (float sy = y + 50; sy < y + 1000; sy += 100)
						for (float sx = x + 50; sx < x + 1000; sx += 100)
						{
							const auto c = land.Cell({sx, sy});
							if (!land.In(c[0], c[1]))
								continue;
							const uint8_t k = land.At(c[0], c[1]);
							landCells += k != kWater;
							beach += k == kBeach;
							urban += k == kUrban || k == kPaved;
						}
					if (landCells < 5)
						continue;
					const char* like = beach * 4 > landCells ? "VesBe3" : urban * 2 > landCells ? "Z_DELPa" : "Z_RHL6";
					const std::string zname = std::format("WB{}_{}", static_cast<int>(x / 1000), static_cast<int>(y / 1000));
					const int at = game.Find(like);
					if (at < 0)
						return error = std::string("no game zone ") + like, false;
					ours.Put(game, at, zname);
					ipl += std::format("{}, {:.1f}, {:.1f}, -100.0, {:.1f}, {:.1f}, 1250.0, {}, 0\n", zname, x, y, x + 1000, y + 1000, zname);
					++stats.zones;
				}
			ipl += "end\n";
			std::string binds;
			if (!ours.Write(binds, error))
				return false;
			std::filesystem::create_directories(o.zonesOut);
			std::ofstream(o.zonesOut / "popzone.ipl", std::ios::binary) << ipl;
			std::ofstream(o.zonesOut / "zonebind.ymt", std::ios::binary).write(binds.data(), static_cast<std::streamsize>(binds.size()));
		}
		return true;
	}
}
