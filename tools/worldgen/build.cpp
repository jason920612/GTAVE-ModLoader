#include "build.hpp"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <format>
#include <fstream>
#include <limits>
#include <map>
#include <set>

#include "../ue5export/drawable.hpp"
#include "bounds.hpp"
#include "buildings.hpp"
#include "convert/pso.hpp"
#include "convert/ytd.hpp"
#include "elevation.hpp"
#include "facades.hpp"
#include "mapfiles.hpp"
#include "navgen.hpp"
#include "paths.hpp"
#include "polygon.hpp"
#include "props.hpp"
#include "raster.hpp"
#include "roads.hpp"
#include "structures.hpp"
#include "terrain.hpp"
#include "texture.hpp"
#include "trees.hpp"
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

		// A texture of the game's (its pixels as stored: format, mips) under a new name, for a dictionary of our own.
		bool CopyTexture(const ue5export::GameFiles& files, const std::string& dictionary, const std::string& texture, const std::string& name,
		    loader::convert::DictionaryTexture& out, std::string& error)
		{
			Bytes data;
			loader::convert::Resource res;
			const auto* f = files.Find(dictionary);
			if (!f || !files.Read(*f, data, error) || !loader::convert::Read(data, res, error))
				return error = "cannot read " + dictionary + ": " + error, false;
			for (const auto& t : ue5export::drawable::Textures(ue5export::drawable::View(res), ue5export::drawable::View::kRoot))
			{
				std::string lower = t.name;
				std::transform(lower.begin(), lower.end(), lower.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
				if (lower != texture)
					continue;
				const bool small = t.dxgi == 71 || t.dxgi == 72 || t.dxgi == 80 || t.dxgi == 81;
				out = {};
				out.name = name;
				out.width = t.width, out.height = t.height, out.depth = 1, out.mips = t.mips;
				out.format = {t.dxgi, small ? 8u : 16u, true};
				out.pixels.assign(t.data.begin(), t.data.end());
				out.map = true;
				return true;
			}
			return error = texture + " is not in " + dictionary, false;
		}

		// A texture of the game's as a picture (its first mip, decoded).
		bool GameImage(const ue5export::GameFiles& files, const std::string& dictionary, const std::string& name, texture::Image& out, std::string& error)
		{
			loader::convert::DictionaryTexture t;
			if (!CopyTexture(files, dictionary, name, name, t, error))
				return false;
			loader::convert::TextureFormat format = t.format;
			uint8_t mips = t.mips;
			Bytes pixels(t.pixels.begin(), t.pixels.end());
			loader::convert::MakeRenderTargetFormat(format, mips, static_cast<uint16_t>(t.width), static_cast<uint16_t>(t.height), pixels);
			out = texture::Image(t.width, t.height);
			for (size_t i = 0; i < out.pixels.size() && 4 * i + 3 < pixels.size(); ++i)
				out.pixels[i] = {pixels[4 * i + 2], pixels[4 * i + 1], pixels[4 * i], pixels[4 * i + 3]};
			return true;
		}

		// The name of the game's texture dictionary with this hash (its file's).
		std::string DictionaryName(const ue5export::GameFiles& files, uint32_t hash)
		{
			for (const auto& [name, f] : files.Files())
				if (name.ends_with(".ytd") && loader::convert::Joaat(name.substr(0, name.size() - 4)) == hash)
					return name.substr(0, name.size() - 4);
			return {};
		}

		// The parameters of the game's map shaders as their files name them.
		const uint32_t kDiffuseTex = loader::convert::Joaat("DiffuseTex"), kBumpTex = loader::convert::Joaat("BumpTex"),
		               kSpecularTex = loader::convert::Joaat("SpecularTex");

		// A building of our own into a tile's meshes (keyed by material: its wall style, `aerialRoof` for the tile's
		// picture or -1, `genericRoof`), moved to around (cx, cy), and its collision triangles. Walls stand on the roof's
		// outline down to below the ground (u along the outline, v up, metres over the style's tile); faces of the roof
		// steeper than about 63 degrees (set-backs, gable ends) are walls too; the rest is roof, from the picture (u, v
		// over the tile from its north-west corner) or the gravel texture (planar).
		void AddBuilding(const structures::Shape& s, const facades::Style& wall, uint32_t wallKey, int aerialRoof, uint32_t genericRoof,
		    const facades::Style& gravel, float cx, float cy, float tx0, float ty1, float tile, uint32_t seed, std::map<uint32_t, roads::Mesh>& meshes,
		    std::vector<P3>& colVertices, std::vector<std::array<uint32_t, 3>>& colTriangles)
		{
			const auto at = [&](uint32_t i) { return P3{static_cast<float>(s.vertices[i][0]), static_cast<float>(s.vertices[i][1]), static_cast<float>(s.vertices[i][2])}; };
			// (ambient light: a little less at the foot of the walls)
			const auto shade = [&](float z) { return static_cast<uint8_t>(255 * std::clamp(0.7f + 0.3f * (z - s.base) / 3, 0.7f, 1.0f)); };
			const auto add = [&](roads::Mesh& m, const P3& p, const P3& n, const P3& t, float u, float v) {
				ydr::Vertex x;
				x.position = {p[0] - cx, p[1] - cy, p[2]};
				x.normal = n;
				x.tangent = {t[0], t[1], t[2], 1};
				x.colour0 = {shade(p[2]), 12, 255, 255};
				x.uv[0] = {u, v};
				m.vertices.push_back(x);
				return static_cast<uint32_t>(m.vertices.size() - 1);
			};
			const auto collide = [&](const P3& a, const P3& b, const P3& c) {
				const uint32_t k = static_cast<uint32_t>(colVertices.size());
				colVertices.insert(colVertices.end(), {a, b, c});
				colTriangles.push_back({k, k + 1, k + 2});
			};
			const float sink = s.base - 0.6f;
			// the outline in order (each corner starts one edge), for u running on round the corners
			std::map<uint32_t, uint32_t> next;
			for (const auto& e : s.outline)
				next[e[0]] = e[1];
			std::set<uint32_t> done;
			float run = static_cast<float>(seed % 1000) / 1000.0f * wall.width;
			roads::Mesh& wm = meshes[wallKey];
			for (const auto& [first, unused] : next)
			{
				if (done.contains(first))
					continue;
				for (uint32_t a = first; !done.contains(a) && next.contains(a); a = next[a])
				{
					done.insert(a);
					const uint32_t b = next[a];
					const P3 pa = at(a), pb = at(b);
					const float dx = pb[0] - pa[0], dy = pb[1] - pa[1], len = std::hypot(dx, dy);
					if (len < 1e-3f)
						continue;
					const P3 n{dy / len, -dx / len, 0}, t{dx / len, dy / len, 0};
					const float u0 = run / wall.width, u1 = (run + len) / wall.width;
					run += len;
					const P3 a0{pa[0], pa[1], sink}, b0{pb[0], pb[1], sink};
					const uint32_t i0 = add(wm, a0, n, t, u0, -(sink - s.base) / wall.height), i1 = add(wm, b0, n, t, u1, -(sink - s.base) / wall.height),
					               i2 = add(wm, pb, n, t, u1, -(pb[2] - s.base) / wall.height), i3 = add(wm, pa, n, t, u0, -(pa[2] - s.base) / wall.height);
					wm.indices.insert(wm.indices.end(), {i0, i1, i2, i0, i2, i3});
					collide(a0, b0, pb);
					collide(a0, pb, pa);
				}
			}
			for (const auto& tri : s.roof)
			{
				const P3 a = at(tri[0]), b = at(tri[1]), c = at(tri[2]);
				const float ux = b[0] - a[0], uy = b[1] - a[1], uz = b[2] - a[2], vx = c[0] - a[0], vy = c[1] - a[1], vz = c[2] - a[2];
				P3 n{uy * vz - uz * vy, uz * vx - ux * vz, ux * vy - uy * vx};
				const float l = std::hypot(n[0], n[1], n[2]);
				if (l < 1e-6f)
					continue;
				n = {n[0] / l, n[1] / l, n[2] / l};
				collide(a, b, c);
				if (n[2] < 0.45f)
				{
					// a steep face: wall, u along its level line
					const float hl = std::max(1e-4f, std::hypot(n[0], n[1]));
					const P3 t{-n[1] / hl, n[0] / hl, 0};
					const auto u = [&](const P3& p) { return (p[0] * t[0] + p[1] * t[1]) / wall.width; };
					const uint32_t i0 = add(wm, a, n, t, u(a), -(a[2] - s.base) / wall.height), i1 = add(wm, b, n, t, u(b), -(b[2] - s.base) / wall.height),
					               i2 = add(wm, c, n, t, u(c), -(c[2] - s.base) / wall.height);
					wm.indices.insert(wm.indices.end(), {i0, i1, i2});
					continue;
				}
				const P3 t{1, 0, 0};
				if (aerialRoof >= 0)
				{
					roads::Mesh& rm = meshes[static_cast<uint32_t>(aerialRoof)];
					const auto uv = [&](const P3& p, int k) { return k == 0 ? (p[0] - tx0) / tile : (ty1 - p[1]) / tile; };
					const uint32_t i0 = add(rm, a, n, t, uv(a, 0), uv(a, 1)), i1 = add(rm, b, n, t, uv(b, 0), uv(b, 1)), i2 = add(rm, c, n, t, uv(c, 0), uv(c, 1));
					rm.indices.insert(rm.indices.end(), {i0, i1, i2});
				}
				else
				{
					roads::Mesh& rm = meshes[genericRoof];
					const uint32_t i0 = add(rm, a, n, t, a[0] / gravel.width, -a[1] / gravel.height), i1 = add(rm, b, n, t, b[0] / gravel.width, -b[1] / gravel.height),
					               i2 = add(rm, c, n, t, c[0] / gravel.width, -c[1] / gravel.height);
					rm.indices.insert(rm.indices.end(), {i0, i1, i2});
				}
			}
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
		// (the world editor's paint over them)
		for (const auto& stroke : w.paint)
		{
			const std::map<std::string, uint8_t> classes{{"water", kWater}, {"land", kLand}, {"beach", kBeach}, {"urban", kUrban}, {"park", kPark}, {"paved", kPaved}};
			const auto it = classes.find(stroke.what);
			if (it == classes.end())
				continue;
			std::vector<P2> line = stroke.points;
			if (line.size() == 1)
				line.push_back(line.front());
			raster::DrawLine(land, line, stroke.radius, it->second);
		}
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
		// The land's water, for the loader (game/sea.cpp; next to the pack's dlc.rpf). Out past the game's water grid the sea
		// is everywhere, under new land too: water.bin says where it really is over this build's terrain, a mask of the land
		// grid's cells (1: water) reaching 8 m in over the shore so that waves still run up the beaches. land.json: that box
		// and how calm the sea round the land is.
		{
			const float tx0 = w.min[0] + std::floor((bx0 - w.min[0]) / o.tile) * o.tile, ty0 = w.min[1] + std::floor((by0 - w.min[1]) / o.tile) * o.tile;
			const float tx1 = w.min[0] + std::ceil((bx1 - w.min[0]) / o.tile) * o.tile, ty1 = w.min[1] + std::ceil((by1 - w.min[1]) / o.tile) * o.tile;
			const uint32_t nx = static_cast<uint32_t>(std::ceil((tx1 - tx0) / land.cell)), ny = static_cast<uint32_t>(std::ceil((ty1 - ty0) / land.cell));
			std::vector<uint8_t> bits((static_cast<size_t>(nx) * ny + 7) / 8, 0);
			constexpr int kReach = 2; // (cells)
			for (uint32_t j = 0; j < ny; ++j)
				for (uint32_t i = 0; i < nx; ++i)
				{
					const int li = static_cast<int>(std::floor((tx0 + (i + 0.5f) * land.cell - land.x0) / land.cell));
					const int lj = static_cast<int>(std::floor((ty0 + (j + 0.5f) * land.cell - land.y0) / land.cell));
					bool water = false;
					for (int dj = -kReach; dj <= kReach && !water; ++dj)
						for (int di = -kReach; di <= kReach && !water; ++di)
						{
							const int a = std::clamp(li + di, 0, land.nx - 1), b = std::clamp(lj + dj, 0, land.ny - 1);
							water = land.At(a, b) == kWater;
						}
					if (water)
						bits[(static_cast<size_t>(j) * nx + i) / 8] |= static_cast<uint8_t>(1u << ((static_cast<size_t>(j) * nx + i) % 8));
				}
			// 'WBWM' version 1: x0, y0, cell (float), nx, ny (uint32), the water's level (float); the bits, rows south to north
			std::ofstream mask(o.out / "water.bin", std::ios::binary);
			const float level = 0, cell = land.cell;
			const uint32_t version = 1;
			mask.write("WBWM", 4);
			mask.write(reinterpret_cast<const char*>(&version), 4);
			mask.write(reinterpret_cast<const char*>(&tx0), 4);
			mask.write(reinterpret_cast<const char*>(&ty0), 4);
			mask.write(reinterpret_cast<const char*>(&cell), 4);
			mask.write(reinterpret_cast<const char*>(&nx), 4);
			mask.write(reinterpret_cast<const char*>(&ny), 4);
			mask.write(reinterpret_cast<const char*>(&level), 4);
			mask.write(reinterpret_cast<const char*>(bits.data()), static_cast<std::streamsize>(bits.size()));
			std::ofstream(o.out / "land.json") << std::format("{{\"box\": [{:.0f}, {:.0f}, {:.0f}, {:.0f}], \"waves\": {:.2f}, \"water\": \"water.bin\", \"climate\": \"{}\"}}\n",
			    tx0, ty0, tx1, ty1, o.waves, w.climate);
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
		// measured ground: the laser scan's ground returns (above the datum) on land where there is a scan; land cells
		// without (under buildings, thick trees) from the measured ones around them
		elevation::Scan scan;
		const bool scanned = !o.scan.empty() && scan.Open(o.scan, o.datum);
		if (!o.scan.empty() && !scanned)
			return error = "no scan rasters in " + o.scan.string(), false;
		if (scanned)
		{
			std::vector<uint8_t> measured(height.v.size(), 0);
			size_t count = 0;
			for (int j = 0; j < land.ny; ++j)
				for (int i = 0; i < land.nx; ++i)
				{
					if (land.At(i, j) == kWater)
						continue;
					const P2 c = land.Centre(i, j);
					float s = 0;
					int n = 0;
					for (int k = 0; k < 16; ++k)
					{
						const float g = scan.Ground(c[0] + ((k & 3) - 1.5f) * land.cell / 4, c[1] + ((k >> 2) - 1.5f) * land.cell / 4);
						if (!std::isnan(g))
							s += g, ++n;
					}
					if (n >= 2)
					{
						height.At(i, j) = std::max(0.25f, s / n);
						measured[static_cast<size_t>(j) * land.nx + i] = 1;
						++count;
					}
				}
			for (int pass = 0; pass < 100; ++pass)
			{
				std::vector<std::pair<size_t, float>> spread;
				for (int j = 1; j + 1 < land.ny; ++j)
					for (int i = 1; i + 1 < land.nx; ++i)
					{
						const size_t k = static_cast<size_t>(j) * land.nx + i;
						if (measured[k] || land.v[k] == kWater)
							continue;
						float s = 0;
						int n = 0;
						for (int dj = -1; dj <= 1; ++dj)
							for (int di = -1; di <= 1; ++di)
							{
								const size_t kk = static_cast<size_t>(j + dj) * land.nx + (i + di);
								if (measured[kk])
									s += height.v[kk], ++n;
							}
						if (n)
							spread.push_back({k, s / n});
					}
				if (spread.empty())
					break;
				for (const auto& [k, v] : spread)
					height.v[k] = v, measured[k] = 1;
			}
			std::printf("ground: %zu land cells of %.0f m measured\n", count, land.cell);
		}
		// the world editor's terrain strokes: raised or lowered by `amount` at the middle of the brush, easing out to its
		// radius; flattened towards the height where the stroke began
		for (const auto& stroke : w.terrain)
		{
			const auto at = [&](const P2& p) {
				const auto c = height.Cell(p);
				return height.In(c[0], c[1]) ? height.At(c[0], c[1]) : 0.0f;
			};
			const float level = at(stroke.points.front());
			float x0 = 1e30f, y0 = 1e30f, x1 = -1e30f, y1 = -1e30f;
			for (const P2& p : stroke.points)
				x0 = std::min(x0, p[0]), y0 = std::min(y0, p[1]), x1 = std::max(x1, p[0]), y1 = std::max(y1, p[1]);
			const auto c0 = height.Cell({x0 - stroke.radius, y0 - stroke.radius}), c1 = height.Cell({x1 + stroke.radius, y1 + stroke.radius});
			for (int j = std::max(0, c0[1]); j <= std::min(height.ny - 1, c1[1]); ++j)
				for (int i = std::max(0, c0[0]); i <= std::min(height.nx - 1, c1[0]); ++i)
				{
					const P2 c = height.Centre(i, j);
					float d = 1e30f;
					for (size_t k = 0; k < stroke.points.size(); ++k)
					{
						const P2& a = stroke.points[k];
						const P2& b = stroke.points[std::min(k + 1, stroke.points.size() - 1)];
						d = std::min(d, polygon::DistanceToSegment(c, a, b));
					}
					if (d >= stroke.radius)
						continue;
					const float weight = Smooth(1 - d / stroke.radius);
					float& h = height.At(i, j);
					if (stroke.what == "raise")
						h += stroke.amount * weight;
					else if (stroke.what == "lower")
						h -= stroke.amount * weight;
					else if (stroke.what == "flatten")
						h += (level - h) * weight;
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
		{
			// (the roads' wear, for checking: kind (0 pothole, 1 patch, 2 broken edge), x, y, z)
			std::ofstream list(o.out.parent_path() / (o.name + "_wear.txt"));
			size_t counts[3]{};
			for (const auto& [kind, at] : surfaces.wear)
			{
				++counts[static_cast<size_t>(kind)];
				list << std::format("{} {:.1f} {:.1f} {:.2f}\n", static_cast<int>(kind), at[0], at[1], at[2]);
			}
			std::printf("roads' wear: %zu potholes, %zu patches, %zu broken edges\n", counts[0], counts[1], counts[2]);
		}
		if (!o.roadsObj.empty())
		{
			// (the surfaces' triangles in the work box, a group each)
			std::ofstream obj(o.roadsObj);
			size_t next = 1;
			std::vector<std::pair<std::string, const roads::Mesh*>> groups;
			for (size_t k = 0; k < paving::kPavings; ++k)
				groups.push_back({std::format("paved{}", k), &surfaces.paved[k]});
			groups.push_back({"worn", &surfaces.worn});
			for (const auto& [group, mesh] : {std::pair<const char*, const roads::Mesh*>{"sidewalk", &surfaces.sidewalk}, {"kerb", &surfaces.kerb},
			         {"markings", &surfaces.markings}, {"potholes", &surfaces.potholes}, {"patches", &surfaces.patches}, {"breakups", &surfaces.breakups}})
				groups.push_back({group, mesh});
			for (const auto& [group, mesh] : groups)
			{
				obj << "g " << group << "\n";
				std::vector<int> index(mesh->vertices.size(), -1);
				for (size_t t = 0; t + 2 < mesh->indices.size(); t += 3)
				{
					const auto& a = mesh->vertices[mesh->indices[t]].position;
					if (a[0] < bx0 || a[0] > bx1 || a[1] < by0 || a[1] > by1)
						continue;
					std::string face = "f";
					for (int k = 0; k < 3; ++k)
					{
						const uint32_t i = mesh->indices[t + k];
						if (index[i] < 0)
						{
							const auto& v = mesh->vertices[i];
							obj << std::format("v {:.2f} {:.2f} {:.2f} {:.3f} {:.3f}\n", v.position[0], v.position[2], -v.position[1], v.uv[0][0], v.uv[0][1]);
							index[i] = static_cast<int>(next++);
						}
						face += " " + std::to_string(index[i]);
					}
					obj << face << "\n";
				}
			}
		}
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

		// buildings: with a scan, of our own (each footprint's measured shape, walls in a style picked by its kind and
		// size); without, the game's models on the footprints (the ground under each no higher than its floor)
		std::vector<structures::Shape> shapes;
		std::vector<int> shapeStyle;
		std::vector<P2> shapeCentre;
		facades::Set facadeSet;
		std::vector<world::Building> found; // (the buildings the scan saw that the map lacks)
		if (o.buildings && scanned)
		{
			found = structures::Missing(scan, w.buildings, {bx0, by0, bx1, by1});
			// (none where the world editor demolished)
			std::erase_if(found, [&](const world::Building& b) { return world::Demolished(w, b.footprint); });
			std::printf("buildings: %zu more that the scan saw and the map has not\n", found.size());
			{
				// (where, for checking: middle x, y, area, height)
				std::ofstream list(o.out.parent_path() / (o.name + "_found.txt"));
				for (const auto& b : found)
				{
					P2 c{0, 0};
					for (const P2& q : b.footprint)
						c[0] += q[0] / b.footprint.size(), c[1] += q[1] / b.footprint.size();
					list << std::format("{:.1f} {:.1f} {:.0f} {:.1f}\n", c[0], c[1], std::fabs(polygon::Area(b.footprint)), b.height);
				}
			}
			facadeSet = facades::Make(o.name + "_f_", 1024);
			structures::Options so;
			structures::Stats ss;
			std::vector<const world::Building*> all;
			for (const auto& b : w.buildings)
				all.push_back(&b);
			for (const auto& b : found)
				all.push_back(&b);
			for (const world::Building* pb : all)
			{
				const world::Building& b = *pb;
				if (b.footprint.size() < 3)
					continue;
				P2 c{0, 0};
				for (const P2& q : b.footprint)
					c[0] += q[0] / b.footprint.size(), c[1] += q[1] / b.footprint.size();
				if (c[0] < bx0 || c[0] >= bx1 || c[1] < by0 || c[1] >= by1 || ground(c[0], c[1]) < 0.3f)
					continue;
				structures::Shape s;
				if (!structures::Make(b, &scan, ground, so, s, ss))
					continue;
				const float area = static_cast<float>(std::fabs(polygon::Area(b.footprint)));
				const uint32_t seed = static_cast<uint32_t>(std::lround(c[0] * 3)) * 73856093u ^ static_cast<uint32_t>(std::lround(c[1] * 3)) * 19349663u;
				shapeStyle.push_back(facades::Pick(facadeSet, b.kind, s.height, area, seed));
				shapeCentre.push_back(c);
				shapes.push_back(std::move(s));
			}
			stats.buildings = shapes.size();
			std::printf("buildings: %zu of our own (%zu measured: %zu flat, %zu shaped; %zu at the map's height; %zu skipped), %zu roof triangles\n",
			    shapes.size(), ss.measured, ss.flat, ss.shaped, ss.mapHeight, ss.skipped, ss.triangles);
		}
		std::vector<buildings::Placement> placed;
		size_t unmatched = 0;
		if (o.buildings && !scanned && !catalog.empty())
		{
			buildings::Matcher matcher(catalog);
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
			{
				// (which models went where, for checking)
				std::map<std::string, int> count;
				for (const auto& p : placed)
					++count[p.item->name];
				std::ofstream list(o.out.parent_path() / (o.name + "_buildings.txt"));
				for (const auto& [name, n] : count)
					list << n << ' ' << name << '\n';
			}
			std::printf("buildings: %zu placed, %zu without a model that fits (%zu models to choose from)\n", placed.size(), unmatched, matcher.Candidates());
		}

		// street furniture and trees: off the asphalt, outside buildings
		std::vector<props::Prop> furniture;
		{
			raster::Grid<uint8_t> blocked;
			blocked.Init(gx0, gy0, gx1, gy1, 2.0f, 0);
			for (const auto& e : network.edges)
				raster::DrawLine(blocked, e.points, e.HalfWidth(), uint8_t{1});
			for (const std::vector<world::Building>* list : {&w.buildings, static_cast<const std::vector<world::Building>*>(&found)})
				for (const auto& b : *list)
					if (!b.footprint.empty() && b.footprint[0][0] >= gx0 && b.footprint[0][0] <= gx1 && b.footprint[0][1] >= gy0 && b.footprint[0][1] <= gy1)
						raster::FillPolygon(blocked, {b.footprint}, uint8_t{1});
			const auto open = [&](float x, float y) {
				const auto c = blocked.Cell({x, y});
				return blocked.In(c[0], c[1]) && !blocked.At(c[0], c[1]);
			};
			// (with a scan the trees stand where it saw them; the map's tree points and the avenues' palms then not)
			props::Place(near, network, surfaces.signals, open, ground, {bx0, by0, bx1, by1}, furniture, !scanned);
			std::printf("props: %zu (palms, street lights, signals)\n", furniture.size());
			if (scanned)
			{
				trees::Stats ts;
				trees::Place(scan, open, ground, {bx0, by0, bx1, by1}, furniture, ts);
				std::printf("trees: %zu palms, %zu broadleaf, %zu bushes\n", ts.palms, ts.broadleaf, ts.bushes);
			}
			{
				// (what stands where, for checking: archetype hash, x, y, z, scale)
				std::ofstream list(o.out.parent_path() / (o.name + "_props.txt"));
				for (const auto& prop : furniture)
					list << std::format("{:08x} {:.1f} {:.1f} {:.1f} {:.2f}\n", prop.archetype, prop.position[0], prop.position[1], prop.position[2], prop.scale);
			}
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
		// buildings of our own: the walls' shader (normal_spec) and the roofs' (default) from a game building; the walls'
		// textures in one dictionary, the parent of each tile's roof picture (gtxd.meta)
		std::vector<ydr::Material> wallMaterials;
		ydr::Material roofMaterial;
		elevation::Aerial aerial;
		const bool pictured = !shapes.empty() && !o.aerial.empty() && aerial.Open(o.aerial);
		const std::string facadeDictionary = o.name + "_facades";
		std::vector<std::pair<std::string, std::string>> parents; // child dictionary, parent
		// the roads' asphalt: a plain one of the game's (the road model's own has a lane line painted in: across our roads
		// and junctions it drew white dashes everywhere), a flat normal map and an even shine; in our dictionary, whose
		// parent is the road model's (its kerb, sidewalk and marking textures)
		std::vector<loader::convert::DictionaryTexture> shared;
		{
			loader::convert::DictionaryTexture t;
			if (!CopyTexture(files, "ch2_roadsa_roads+hi.ytd", "im_road_008_new", o.name + "_asphalt", t, error))
				return false;
			shared.push_back(std::move(t));
			shared.push_back(texture::Make(o.name + "_flat_n", texture::Image(4, 4, {128, 128, 255, 255}), texture::Format::Bc1));
			shared.push_back(texture::Make(o.name + "_asphalt_s", texture::Image(4, 4, {48, 48, 48, 255}), texture::Format::Bc1));
			asphalt.Texture(kDiffuseTex, o.name + "_asphalt");
			asphalt.Texture(kBumpTex, o.name + "_flat_n");
			asphalt.Texture(kSpecularTex, o.name + "_asphalt_s");
			const std::string roadDictionary = DictionaryName(files, roadTypes.textureDictionary);
			if (roadDictionary.empty())
				return error = "the road model's texture dictionary has no file", false;
			parents.push_back({facadeDictionary, roadDictionary});
		}
		// the other pavings (paving.hpp) the roads here are made of: the asphalt's shader with the game's textures for
		// them (and the asphalt's even dull shine: their own specular maps read as wet); the wear's decals: the game's
		// road decal shader (a road model's patch decal) with its pothole and crumbled-edge pictures and a patch picture
		// of our own, shining as the asphalt does (the template's sharp highlight looked wet). Copied into our
		// dictionary, only what is used.
		std::array<ydr::Material, paving::kPavings> pavedMaterials;
		pavedMaterials.fill(asphalt);
		ydr::Material wornMaterial = asphalt; // (worn asphalt: the game's cracked road picture and its relief)
		ydr::Material patchDecal, potholeDecal, breakupDecal;
		{
			std::map<std::pair<std::string, std::string>, std::string> copied; // (dictionary, texture) -> our name
			const auto use = [&](const char* dictionary, const char* texture, std::string& name) {
				const auto key = std::make_pair(std::string(dictionary), std::string(texture));
				if (const auto it = copied.find(key); it != copied.end())
					return name = it->second, true;
				loader::convert::DictionaryTexture t;
				name = std::format("{}_{}", o.name, texture);
				if (!CopyTexture(files, dictionary, texture, name, t, error))
					return false;
				shared.push_back(std::move(t));
				copied[key] = name;
				return true;
			};
			struct Look
			{
				const char *dictionary, *diffuse, *normalDictionary, *normal, *specularDictionary, *specular; // (null: flat, the asphalt's dull)
			};
			const Look looks[paving::kPavings] = {
			    {},
			    {"portblockgroup2b+hi.ytd", "im_freeway_road001", "sp1_rd.ytd", "im_freeway_road001_n", nullptr, nullptr},
			    {"hw1_15_details.ytd", "im_ground_bricks03", "ss1_05_ground.ytd", "im_ground_bricks03_n", nullptr, nullptr},
			    {"dt1_20_build+hi.ytd", "rsn_dc_archedcobbles_0001", "kt1_15_ground01+hidr.ytd", "rsn_dc_archedcobbles_0001_n", nullptr, nullptr},
			    {"vb_38_ground+hi.ytd", "rsn_dc_rd_slabs_0001", "vb_38_ground.ytd", "rsn_dc_rd_slabs_0001_n", nullptr, nullptr},
			    {"venicebeachrd+hi.ytd", "dc_gravel_road02", "cs1_09_decals_06+hidr.ytd", "dc_gravel_road02_n", nullptr, nullptr},
			    {"cs3_01_beach3+hidr.ytd", "cs_rsn_sm_dirtroad_01", "cs3_01_beach3+hidr.ytd", "cs_rsn_sm_dirtroad_01_n", nullptr, nullptr},
			    {"cs3_01_beach3+hidr.ytd", "cs_rsn_sm_dirtroad_01", "cs3_01_beach3+hidr.ytd", "cs_rsn_sm_dirtroad_01_n", nullptr, nullptr}};
			if (!surfaces.worn.indices.empty())
			{
				std::string diffuse, normal;
				if (!use("po1_rd_po1_rd+hi.ytd", "im_road_012", diffuse) || !use("po1_rd_po1_rd+hi.ytd", "im_road_012_n", normal))
					return false;
				wornMaterial.Texture(kDiffuseTex, diffuse);
				wornMaterial.Texture(kBumpTex, normal);
			}
			for (size_t k = 1; k < paving::kPavings; ++k)
			{
				if (surfaces.paved[k].indices.empty())
					continue;
				const Look& look = looks[k];
				std::string diffuse, normal = o.name + "_flat_n", specular = o.name + "_asphalt_s";
				if (!use(look.dictionary, look.diffuse, diffuse) || (look.normal && !use(look.normalDictionary, look.normal, normal)) ||
				    (look.specular && !use(look.specularDictionary, look.specular, specular)))
					return false;
				pavedMaterials[k].Texture(kDiffuseTex, diffuse);
				pavedMaterials[k].Texture(kBumpTex, normal);
				pavedMaterials[k].Texture(kSpecularTex, specular);
			}
			if (!surfaces.patches.indices.empty() || !surfaces.potholes.indices.empty() || !surfaces.breakups.indices.empty())
			{
				ydr::Material decal;
				if (!GameMaterial(files, "ap1_01_b_sidebit1b001.ydr", 2, decal, error))
					return false;
				const auto make = [&](ydr::Material& m, const char* dictionary, const char* diffuse, const char* normal, const char* specular) {
					std::string d, n = o.name + "_flat_n", s = o.name + "_asphalt_s";
					if (!use(dictionary, diffuse, d) || (normal && !use(dictionary, normal, n)) || (specular && !use(dictionary, specular, s)))
						return false;
					m = decal;
					m.Texture(kDiffuseTex, d);
					m.Texture(kBumpTex, n);
					m.Texture(kSpecularTex, s);
					return true;
				};
				// (Fresnel; Specular: the highlight's tightness; SpecularColor: its strength - as the asphalt's)
				decal.Constant(loader::convert::Joaat("fresnel"), 0.95f);
				decal.Constant(loader::convert::Joaat("specular"), 16.0f);
				decal.Constant(loader::convert::Joaat("specularcolor"), 1.0f);
				if (!make(potholeDecal, "ap1_01_b_sidebit1b001+hidr.ytd", "et_pothole1", nullptr, nullptr) ||
				    !make(breakupDecal, "vb_34_seawall+hi.ytd", "im_road_damage_05", "im_road_damage_05_n", nullptr))
					return false;
				// a patch: newer (darker) asphalt with the dark sealed seam of its cut all round; the picture is the whole
				// patch (the game's patch pictures are opaque all over, shaped by their meshes)
				{
					texture::Image fresh;
					if (!GameImage(files, "ap1_04_roads00+hidr.ytd", "im_road_009", fresh, error))
						return false;
					texture::Image patch(256, 512);
					for (int y = 0; y < patch.height; ++y)
						for (int x = 0; x < patch.width; ++x)
						{
							const auto& c = fresh.At((x * 2) % fresh.width, y % fresh.height);
							const int seam = std::min({x, patch.width - 1 - x, y / 2, (patch.height - 1 - y) / 2});
							const float k = seam < 3 ? 0.35f : seam < 5 ? 0.6f : 0.8f;
							patch.At(x, y) = {static_cast<uint8_t>(c[0] * k), static_cast<uint8_t>(c[1] * k), static_cast<uint8_t>(c[2] * k), 255};
						}
					shared.push_back(texture::Make(o.name + "_patch", patch, texture::Format::Bc1));
					shared.push_back(texture::Make(o.name + "_patch_s", texture::Image(4, 4, {34, 34, 34, 255}), texture::Format::Bc1));
					patchDecal = decal;
					patchDecal.Texture(kDiffuseTex, o.name + "_patch");
					patchDecal.Texture(kBumpTex, o.name + "_flat_n");
					patchDecal.Texture(kSpecularTex, o.name + "_patch_s");
				}
			}
		}
		if (!shapes.empty())
		{
			ydr::Material wallTemplate;
			if (!GameMaterial(files, "sm_14_bld2.ydr", 6, wallTemplate, error) || !GameMaterial(files, "sm_14_bld2.ydr", 1, roofMaterial, error))
				return false;
			for (const auto& style : facadeSet.styles)
			{
				ydr::Material m = wallTemplate;
				m.Texture(kDiffuseTex, style.diffuse);
				m.Texture(kBumpTex, style.normal);
				m.Texture(kSpecularTex, style.specular);
				wallMaterials.push_back(m);
			}
			shared.insert(shared.end(), facadeSet.textures.begin(), facadeSet.textures.end());
			if (!o.aerial.empty() && !pictured)
				std::printf("warning: no aerial pictures in %s (roofs get the gravel texture)\n", o.aerial.string().c_str());
		}
		{
			Bytes txd;
			if (!loader::convert::WriteTextureDictionary(shared, txd, error))
				return error = facadeDictionary + ": " + error, false;
			WriteFile(o.out / (facadeDictionary + ".ytd"), txd);
		}
		const int gravelStyle = shapes.empty() ? 0 : facades::Find(facadeSet, facades::Kind::Roof);
		const int per = std::max(1, static_cast<int>(std::lround(o.tile / o.cell)));
		const int ti0 = static_cast<int>(std::floor((bx0 - w.min[0]) / o.tile)), tj0 = static_cast<int>(std::floor((by0 - w.min[1]) / o.tile));
		const int ti1 = static_cast<int>(std::ceil((bx1 - w.min[0]) / o.tile)), tj1 = static_cast<int>(std::ceil((by1 - w.min[1]) / o.tile));
		std::vector<mapfiles::Archetype> archetypes;
		std::map<std::pair<int, int>, std::vector<mapfiles::Entity>> blocks; // 1 km blocks of entities
		std::map<std::pair<int, int>, std::vector<mapfiles::Entity>> lodBlocks; // their LOD maps' (the buildings' LODs)
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
				// (what each road triangle is to the pedestrians' navmesh, carriageway or sidewalk: by part)
				std::map<size_t, std::vector<uint8_t>> roadKinds;
				ybn::Part part;
				std::vector<uint8_t> partKinds;
				std::map<uint8_t, uint8_t> materialAt;
				const auto flush = [&] {
					if (!part.triangles.empty())
					{
						roadKinds[parts.size()] = std::move(partKinds);
						parts.push_back(std::move(part));
					}
					part = {};
					partKinds.clear();
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
					partKinds.push_back(col.kinds[t]);
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
				// buildings of our own standing in this tile (by their middles): one model; their triangles as collision and
				// as the navmesh's obstacles
				if (!shapes.empty())
				{
					std::map<uint32_t, roads::Mesh> meshes;
					std::vector<P3> cv;
					std::vector<std::array<uint32_t, 3>> ct;
					const uint32_t aerialKey = static_cast<uint32_t>(facadeSet.styles.size()), gravelKey = static_cast<uint32_t>(gravelStyle);
					float tallest = 0;
					size_t standing = 0;
					for (size_t k = 0; k < shapes.size(); ++k)
					{
						const P2& c = shapeCentre[k];
						if (c[0] < x0 || c[0] >= x1 || c[1] < y0 || c[1] >= y1)
							continue;
						const auto& s = shapes[k];
						// (the picture's roofs: low buildings that are there - a tall one leans in it, its roof drawn off its footprint;
						// the world editor's new ones are not in it yet)
						const bool fromPicture = pictured && s.top < 25 && s.measured;
						AddBuilding(s, facadeSet.styles[shapeStyle[k]], static_cast<uint32_t>(shapeStyle[k]), fromPicture ? static_cast<int>(aerialKey) : -1, gravelKey,
						    facadeSet.styles[gravelStyle], cx, cy, x0, y1, o.tile, static_cast<uint32_t>(k), meshes, cv, ct);
						tallest = std::max(tallest, s.top);
						++standing;
					}
					if (standing)
					{
						const std::string bname = std::format("{}_b_{}_{}", o.name, ti, tj);
						std::string txd = facadeDictionary;
						texture::Image lodRoof;
						if (meshes.contains(aerialKey))
						{
							// the tile's roof picture
							texture::Image img(o.roofTexture, o.roofTexture);
							bool any = false;
							for (int y = 0; y < img.height; ++y)
								for (int x = 0; x < img.width; ++x)
								{
									std::array<float, 4> px;
									const float wx = x0 + (x + 0.5f) * o.tile / img.width, wy = y1 - (y + 0.5f) * o.tile / img.height;
									if (aerial.Sample(wx, wy, px))
									{
										img.At(x, y) = {static_cast<uint8_t>(px[0]), static_cast<uint8_t>(px[1]), static_cast<uint8_t>(px[2]), 255};
										any = true;
									}
									else
										img.At(x, y) = {150, 150, 146, 255};
								}
							if (any)
							{
								// (the LOD's: a quarter of the size)
								lodRoof = texture::Image(img.width / 4, img.height / 4);
								for (int y = 0; y < lodRoof.height; ++y)
									for (int x = 0; x < lodRoof.width; ++x)
										for (int ch = 0; ch < 4; ++ch)
										{
											int sum = 0;
											for (int k = 0; k < 16; ++k)
												sum += img.At(x * 4 + (k & 3), y * 4 + (k >> 2))[ch];
											lodRoof.At(x, y)[ch] = static_cast<uint8_t>(sum / 16);
										}
								txd = std::format("{}_rt_{}_{}", o.name, ti, tj);
								Bytes ytd;
								if (!loader::convert::WriteTextureDictionary({texture::Make(std::format("{}_roof_{}_{}", o.name, ti, tj), img, texture::Format::Bc1)}, ytd, error))
									return error = txd + ": " + error, false;
								WriteFile(o.out / (txd + ".ytd"), ytd);
								parents.push_back({txd, facadeDictionary});
							}
							else
							{
								// (no picture after all: the gravel)
								auto& from = meshes[aerialKey];
								auto& to = meshes[gravelKey];
								const uint32_t base = static_cast<uint32_t>(to.vertices.size());
								for (auto v : from.vertices)
								{
									v.uv[0] = {(v.position[0] + cx) / facadeSet.styles[gravelStyle].width, -(v.position[1] + cy) / facadeSet.styles[gravelStyle].height};
									to.vertices.push_back(v);
								}
								for (const uint32_t i : from.indices)
									to.indices.push_back(base + i);
								meshes.erase(aerialKey);
							}
						}
						ydr::Drawable bd;
						bd.name = bname;
						for (const auto& [key, mesh] : meshes)
						{
							if (key == aerialKey)
							{
								ydr::Material m = roofMaterial;
								m.Texture(kDiffuseTex, std::format("{}_roof_{}_{}", o.name, ti, tj));
								bd.materials.push_back(m);
							}
							else
								bd.materials.push_back(wallMaterials[key]);
							AddMeshes(bd, static_cast<uint32_t>(bd.materials.size() - 1), mesh);
						}
						if (!ydr::Write(bd, file, error))
							return error = bname + ": " + error, false;
						WriteFile(o.out / (bname + ".ydr"), file);
						mapfiles::Archetype ba;
						ba.name = bname;
						ba.textureDictionary = loader::convert::Joaat(txd);
						// (near: the full model; past it its LOD - one material, small pictures - out to the skyline's
						// distance, as the game's own maps do: the HD entity names its LOD in the block's LOD map)
						ba.lodDist = std::clamp(300 + tallest * 6, 300.0f, 900.0f);
						ba.hdTextureDist = 300;
						ydr::Box(bd, ba.min, ba.max);
						archetypes.push_back(ba);
						mapfiles::Entity be;
						be.archetype = loader::convert::Joaat(bname);
						be.position = {cx, cy, 0};
						be.lodDist = ba.lodDist;
						be.radius = std::hypot(ba.max[0] - ba.min[0], ba.max[1] - ba.min[1], ba.max[2] - ba.min[2]) / 2;
						if (o.lods)
						{
							// the LOD model: the same walls (their textures are the shared dictionary's, there already), the roofs'
							// picture a quarter of the size, small sheds left out
							std::map<uint32_t, roads::Mesh> lodMeshes;
							std::vector<P3> unusedV;
							std::vector<std::array<uint32_t, 3>> unusedT;
							for (size_t k = 0; k < shapes.size(); ++k)
							{
								const P2& c = shapeCentre[k];
								if (c[0] < x0 || c[0] >= x1 || c[1] < y0 || c[1] >= y1)
									continue;
								const auto& s = shapes[k];
								double area = 0;
								for (const auto& t : s.roof)
								{
									const auto& a = s.vertices[t[0]];
									const auto& b2 = s.vertices[t[1]];
									const auto& c2 = s.vertices[t[2]];
									area += ((b2[0] - a[0]) * (c2[1] - a[1]) - (b2[1] - a[1]) * (c2[0] - a[0])) / 2;
								}
								if (area < 40 && s.top < 6)
									continue;
								const bool fromPicture = lodRoof.width > 0 && s.top < 25;
								AddBuilding(s, facadeSet.styles[shapeStyle[k]], static_cast<uint32_t>(shapeStyle[k]), fromPicture ? static_cast<int>(aerialKey) : -1,
								    gravelKey, facadeSet.styles[gravelStyle], cx, cy, x0, y1, o.tile, static_cast<uint32_t>(k), lodMeshes, unusedV, unusedT);
							}
							const std::string lname = std::format("{}_bl_{}_{}", o.name, ti, tj);
							ydr::Drawable ld;
							ld.name = lname;
							std::string ltxd = facadeDictionary;
							for (const auto& [key, mesh] : lodMeshes)
							{
								if (mesh.indices.empty())
									continue;
								if (key == aerialKey)
								{
									ltxd = std::format("{}_rl_{}_{}", o.name, ti, tj);
									const std::string rname2 = std::format("{}_roofl_{}_{}", o.name, ti, tj);
									Bytes ytd;
									if (!loader::convert::WriteTextureDictionary({texture::Make(rname2, lodRoof, texture::Format::Bc1)}, ytd, error))
										return error = ltxd + ": " + error, false;
									WriteFile(o.out / (ltxd + ".ytd"), ytd);
									parents.push_back({ltxd, facadeDictionary});
									ydr::Material m = roofMaterial;
									m.Texture(kDiffuseTex, rname2);
									ld.materials.push_back(m);
								}
								else
									ld.materials.push_back(wallMaterials[key]);
								AddMeshes(ld, static_cast<uint32_t>(ld.materials.size() - 1), mesh);
							}
							if (!ld.materials.empty())
							{
								if (!ydr::Write(ld, file, error))
									return error = lname + ": " + error, false;
								WriteFile(o.out / (lname + ".ydr"), file);
								mapfiles::Archetype la;
								la.name = lname;
								la.textureDictionary = loader::convert::Joaat(ltxd);
								la.lodDist = std::clamp(1500 + tallest * 40, 1500.0f, 6000.0f);
								la.hdTextureDist = 0;
								ydr::Box(ld, la.min, la.max);
								archetypes.push_back(la);
								mapfiles::Entity le;
								le.archetype = loader::convert::Joaat(lname);
								le.position = {cx, cy, 0};
								le.lodDist = la.lodDist;
								le.lodLevel = 1;
								le.numChildren = 1;
								le.childLodDist = be.lodDist;
								le.radius = be.radius;
								auto& lods = lodBlocks[{bi, bj}];
								be.lodLevel = 0;
								be.parentIndex = static_cast<int32_t>(lods.size());
								lods.push_back(le);
							}
						}
						blocks[{bi, bj}].push_back(be);
						stats.generatedTiles += 1;
						// collision (concrete), in parts of at most 1000 triangles
						for (size_t first = 0; first < ct.size(); first += 900)
						{
							ybn::Part bp;
							bp.materials.push_back(ybn::Material(kConcrete));
							for (size_t t = first; t < std::min(ct.size(), first + 900); ++t)
							{
								const uint32_t b0 = static_cast<uint32_t>(bp.vertices.size());
								for (int k = 0; k < 3; ++k)
									bp.vertices.push_back(cv[ct[t][k]]);
								bp.triangles.push_back({b0, b0 + 1, b0 + 2});
								bp.triangleMaterials.push_back(0);
							}
							parts.push_back(std::move(bp));
						}
						const uint32_t navBase = static_cast<uint32_t>(boxVertices.size());
						boxVertices.insert(boxVertices.end(), cv.begin(), cv.end());
						for (const auto& t : ct)
							boxTriangles.push_back({navBase + t[0], navBase + t[1], navBase + t[2]});
					}
				}
				// the street furniture in this tile (props: their own collision)
				for (const auto& prop : furniture)
				{
					if (prop.position[0] < x0 || prop.position[0] >= x1 || prop.position[1] < y0 || prop.position[1] >= y1)
						continue;
					mapfiles::Entity pe;
					pe.archetype = prop.archetype;
					pe.position = prop.position;
					pe.rotation = {0, 0, std::sin(prop.heading / 2), std::cos(prop.heading / 2)};
					pe.lodDist = prop.lodDist;
					pe.scaleXY = pe.scaleZ = prop.scale;
					pe.radius = 6 * prop.scale;
					blocks[{bi, bj}].push_back(pe);
				}
				// (a tile's collision: at most 256 parts)
				if (parts.size() > 256)
				{
					std::printf("warning: tile %d,%d has %zu collision parts (256 kept)\n", ti, tj, parts.size());
					parts.resize(256);
				}
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
				for (size_t pi = 0; pi < parts.size(); ++pi)
				{
					const ybn::Part& p = parts[pi];
					const auto kinds = roadKinds.find(pi);
					const uint32_t base = static_cast<uint32_t>(navMesh.vertices.size());
					navMesh.vertices.insert(navMesh.vertices.end(), p.vertices.begin(), p.vertices.end());
					for (size_t t = 0; t < p.triangles.size(); ++t)
					{
						if (boxSet.contains({p.vertices[p.triangles[t][0]], p.vertices[p.triangles[t][1]], p.vertices[p.triangles[t][2]]}))
							continue; // (a building's: added above)
						navMesh.triangles.push_back({base + p.triangles[t][0], base + p.triangles[t][1], base + p.triangles[t][2]});
						const uint8_t m = p.materials[p.triangleMaterials[t]][0];
						navMesh.materials.push_back(m);
						const uint8_t kind = kinds != roadKinds.end() ? kinds->second[t] : 0;
						navSurfaces.push_back(kind == 1 ? navgen::Surface::Road
						                      : kind == 2 || (kind == 0 && (m == 1 || m == 15 || m == 174)) ? navgen::Surface::Pavement
						                                                                                    : navgen::Surface::Ground);
					}
				}
				// the roads' model
				// (its materials those it uses here; the wear's decals before the markings, which are painted over them)
				ydr::Drawable rd;
				rd.name = rname;
				const auto add = [&](const ydr::Material& mat, const roads::Mesh& mesh) {
					const roads::Mesh part = Clip(mesh, x0, y0, x1, y1, cx, cy);
					if (part.indices.empty())
						return;
					rd.materials.push_back(mat);
					AddMeshes(rd, static_cast<uint32_t>(rd.materials.size() - 1), part);
				};
				for (size_t k = 0; k < paving::kPavings; ++k)
					add(pavedMaterials[k], surfaces.paved[k]);
				add(wornMaterial, surfaces.worn);
				add(sidewalk, surfaces.sidewalk);
				add(kerb, surfaces.kerb);
				add(patchDecal, surfaces.patches);
				add(potholeDecal, surfaces.potholes);
				add(breakupDecal, surfaces.breakups);
				add(marking, surfaces.markings);
				if (!rd.lods[0].empty())
				{
					if (!ydr::Write(rd, file, error))
						return error = rname + ": " + error, false;
					WriteFile(o.out / (rname + ".ydr"), file);
					mapfiles::Archetype ra;
					ra.name = rname;
					ra.textureDictionary = loader::convert::Joaat(facadeDictionary);
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
		// the roof pictures' dictionaries under the walls' (worldgen pack puts gtxd.meta in the pack's data)
		if (!parents.empty())
		{
			std::ofstream g(o.out / "gtxd.meta", std::ios::binary);
			g << "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n<CMapParentTxds>\n  <txdRelationships>\n";
			for (const auto& [child, parent] : parents)
				g << "    <Item>\n      <parent>" << parent << "</parent>\n      <child>" << child << "</child>\n    </Item>\n";
			g << "  </txdRelationships>\n</CMapParentTxds>\n";
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
		// (in files of at most 1900 entries: an array's pointers must fit one 16 KB page of the meta resource)
		constexpr size_t kPerFile = 1900;
		for (size_t first = 0, k = 0; first < archetypes.size(); first += kPerFile, ++k)
		{
			const std::vector<mapfiles::Archetype> part(archetypes.begin() + static_cast<ptrdiff_t>(first),
			    archetypes.begin() + static_cast<ptrdiff_t>(std::min(archetypes.size(), first + kPerFile)));
			const std::string tname = k ? std::format("{}_{}", o.name, k) : o.name;
			Bytes types;
			if (!mapfiles::WriteTypes(files, tname, part, types, error))
				return false;
			WriteFile(o.out / (tname + ".ytyp"), types);
		}
		// (a block's LOD map: one file - its entities are the HD maps' parents by index)
		for (const auto& [block, entities] : lodBlocks)
		{
			if (entities.size() > 0xFFFF)
				return error = "a block's LOD map is too big", false;
			Bytes map;
			const std::string mname = std::format("{}_lod_{}_{}", o.name, block.first, block.second);
			if (!mapfiles::WriteMap(files, mname, entities, map, error, {0, 2, 2}))
				return false;
			WriteFile(o.out / (mname + ".ymap"), map);
		}
		// The maps, by how far their entities are seen (a map streams in once the camera is within its entities' lodDist of
		// them, so near things must not share a map with far ones): far (terrain, roads: lodDist 1 km and more) in 1 km
		// blocks; the buildings' HD models (with a LOD parent) in maps of their own per block, children of the block's LOD
		// map - mixed with parentless entities, the game took those for coarser objects and looked for the buildings'
		// parents among them (their parentIndex named a terrain tile, hidden while the buildings showed: a hole near the
		// camera; research/phase0.md §61.21); near (trees, props: a few hundred metres) in 250 m cells, so that only those
		// round the camera are loaded (a whole city's trees would not fit the game's entity pools).
		std::map<std::pair<int, int>, std::vector<mapfiles::Entity>> nearCells;
		const auto writeMaps = [&](const std::string& base, const std::vector<mapfiles::Entity>& entities, const mapfiles::MapInfo& info) {
			for (size_t first = 0, k = 0; first < entities.size(); first += kPerFile, ++k)
			{
				const std::vector<mapfiles::Entity> part(entities.begin() + static_cast<ptrdiff_t>(first),
				    entities.begin() + static_cast<ptrdiff_t>(std::min(entities.size(), first + kPerFile)));
				Bytes map;
				const std::string mname = k ? std::format("{}_{}", base, k) : base;
				if (!mapfiles::WriteMap(files, mname, part, map, error, info))
					return false;
				WriteFile(o.out / (mname + ".ymap"), map);
			}
			return true;
		};
		for (const auto& [block, all] : blocks)
		{
			std::vector<mapfiles::Entity> far, children;
			for (const auto& e : all)
			{
				if (e.parentIndex >= 0)
					children.push_back(e);
				else if (e.lodDist >= 1000)
					far.push_back(e);
				else
					nearCells[{static_cast<int>(std::floor((e.position[0] - w.min[0]) / 250)), static_cast<int>(std::floor((e.position[1] - w.min[1]) / 250))}]
					    .push_back(e);
			}
			mapfiles::MapInfo hd;
			hd.parent = loader::convert::Joaat(std::format("{}_lod_{}_{}", o.name, block.first, block.second));
			if (!writeMaps(std::format("{}_{}_{}", o.name, block.first, block.second), far, {}) ||
			    !writeMaps(std::format("{}_hd_{}_{}", o.name, block.first, block.second), children, hd))
				return false;
		}
		for (const auto& [cell, entities] : nearCells)
			if (!writeMaps(std::format("{}_n_{}_{}", o.name, cell.first, cell.second), entities, {}))
				return false;
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
