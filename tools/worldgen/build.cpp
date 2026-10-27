#include "build.hpp"

#include <algorithm>
#include <thread>
#include <mutex>
#include <atomic>
#include <cctype>
#include <chrono>
#include <cmath>
#include <cstring>
#include <format>
#include <fstream>
#include <limits>
#include <map>
#include <set>
#include <unordered_map>

#include "../ue5export/drawable.hpp"
#include "bounds.hpp"
#include "buildings.hpp"
#include "convert/pso.hpp"
#include "convert/ytd.hpp"
#include "elevation.hpp"
#include "facades.hpp"
#include "mapfiles.hpp"
#include "minimap.hpp"
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
		constexpr uint8_t kGrass = 47, kSand = 19, kConcrete = 1, kDirt = 43;

		float Smooth(float t)
		{
			t = std::clamp(t, 0.0f, 1.0f);
			return t * t * (3 - 2 * t);
		}

		// Value noise on a lattice of `cell` (smoothed), -1..1; `period` lattice cells: it repeats (0: never).
		float Lattice(float x, float y, float cell, uint32_t seed, int period = 0)
		{
			const auto at = [&](int i, int j) {
				if (period)
					i = ((i % period) + period) % period, j = ((j % period) + period) % period;
				uint32_t h = static_cast<uint32_t>(i) * 73856093u ^ static_cast<uint32_t>(j) * 19349663u ^ seed * 83492791u;
				h ^= h >> 16, h *= 0x7feb352du, h ^= h >> 15, h *= 0x846ca68bu, h ^= h >> 16;
				return static_cast<float>(h >> 8) / 8388608.0f - 1.0f;
			};
			const float fx = x / cell, fy = y / cell;
			const int i = static_cast<int>(std::floor(fx)), j = static_cast<int>(std::floor(fy));
			float tx = fx - i, ty = fy - j;
			tx = tx * tx * (3 - 2 * tx), ty = ty * ty * (3 - 2 * ty);
			return (at(i, j) * (1 - tx) + at(i + 1, j) * tx) * (1 - ty) + (at(i, j + 1) * (1 - tx) + at(i + 1, j + 1) * tx) * ty;
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

		// Things by the tile their position is in (from the world box's corner, tiles `size` m): a tile's are looked up in
		// its cell and the eight round it - a thing on a tile's edge is still kept or left by the same test as before - in
		// the order they were added (what the tiles hold, and in what order, as when each tile went through all of them).
		struct TileIndex
		{
			float x0 = 0, y0 = 0, size = 1;
			int ti0 = 0, tj0 = 0, nx = 0, ny = 0;
			std::vector<std::vector<uint32_t>> cells;

			TileIndex(float x0_, float y0_, float size_, int ti0_, int tj0_, int ti1, int tj1)
			    : x0(x0_), y0(y0_), size(size_), ti0(ti0_ - 1), tj0(tj0_ - 1), nx(ti1 - ti0_ + 2), ny(tj1 - tj0_ + 2)
			{
				cells.resize(static_cast<size_t>(nx) * ny);
			}
			void Add(uint32_t thing, float x, float y)
			{
				const int i = std::clamp(static_cast<int>(std::floor((x - x0) / size)) - ti0, 0, nx - 1);
				const int j = std::clamp(static_cast<int>(std::floor((y - y0) / size)) - tj0, 0, ny - 1);
				cells[static_cast<size_t>(j) * nx + i].push_back(thing);
			}
			std::vector<uint32_t> Near(int ti, int tj) const
			{
				std::vector<uint32_t> out;
				for (int j = tj - 1; j <= tj + 1; ++j)
					for (int i = ti - 1; i <= ti + 1; ++i)
					{
						const int ci = i - ti0, cj = j - tj0;
						if (ci >= 0 && cj >= 0 && ci < nx && cj < ny)
							out.insert(out.end(), cells[static_cast<size_t>(cj) * nx + ci].begin(), cells[static_cast<size_t>(cj) * nx + ci].end());
					}
				std::sort(out.begin(), out.end());
				return out;
			}
		};

		// A mesh's triangles by tile (their centres).
		TileIndex MeshTiles(const roads::Mesh& m, float x0, float y0, float size, int ti0, int tj0, int ti1, int tj1)
		{
			TileIndex index(x0, y0, size, ti0, tj0, ti1, tj1);
			for (size_t t = 0; t + 2 < m.indices.size(); t += 3)
			{
				const auto& a = m.vertices[m.indices[t]].position;
				const auto& b = m.vertices[m.indices[t + 1]].position;
				const auto& c = m.vertices[m.indices[t + 2]].position;
				index.Add(static_cast<uint32_t>(t / 3), (a[0] + b[0] + c[0]) / 3, (a[1] + b[1] + c[1]) / 3);
			}
			return index;
		}

		// A mesh's triangles in one tile (by their centres), moved to around (cx, cy); of `candidates` (triangle numbers, in
		// order) when given, else of all.
		roads::Mesh Clip(const roads::Mesh& m, float x0, float y0, float x1, float y1, float cx, float cy, const std::vector<uint32_t>* candidates = nullptr)
		{
			roads::Mesh out;
			std::map<uint32_t, uint32_t> remap;
			const size_t count = candidates ? candidates->size() : m.indices.size() / 3;
			for (size_t n = 0; n < count; ++n)
			{
				const size_t t = (candidates ? (*candidates)[n] : n) * size_t{3};
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

		// A ground texture with what changes across it at large - patches, a lighter corner - taken out: each channel
		// divided by its blur over about an eighth of the side (wrapping: it repeats), times its mean. Repeated every few
		// metres those patches were a grid of squares from a little way off; the ground's larger changes come from the
		// vertices' noise and the blend map, which do not repeat.
		void Flatten(texture::Image& image)
		{
			const int w = image.width, h = image.height, rx = std::max(1, w / 16), ry = std::max(1, h / 16);
			const auto wrap = [](int v, int n) { return ((v % n) + n) % n; };
			std::vector<float> a(static_cast<size_t>(w) * h), b(a.size());
			std::array<std::vector<float>, 3> blur;
			std::array<double, 3> mean{};
			for (int c = 0; c < 3; ++c)
			{
				mean[c] = 0;
				for (size_t k = 0; k < a.size(); ++k)
					a[k] = image.pixels[k][c], mean[c] += a[k];
				mean[c] /= static_cast<double>(a.size());
				// (three box passes each way: about a Gaussian)
				for (int pass = 0; pass < 3; ++pass)
				{
					for (int y = 0; y < h; ++y)
					{
						float* row = &a[static_cast<size_t>(y) * w];
						float s = 0;
						for (int x = -rx; x <= rx; ++x)
							s += row[wrap(x, w)];
						for (int x = 0; x < w; ++x)
						{
							b[static_cast<size_t>(y) * w + x] = s / (2 * rx + 1);
							s += row[wrap(x + rx + 1, w)] - row[wrap(x - rx, w)];
						}
					}
					for (int x = 0; x < w; ++x)
					{
						float s = 0;
						for (int y = -ry; y <= ry; ++y)
							s += b[static_cast<size_t>(wrap(y, h)) * w + x];
						for (int y = 0; y < h; ++y)
						{
							a[static_cast<size_t>(y) * w + x] = s / (2 * ry + 1);
							s += b[static_cast<size_t>(wrap(y + ry + 1, h)) * w + x] - b[static_cast<size_t>(wrap(y - ry, h)) * w + x];
						}
					}
				}
				blur[c] = a;
			}
			for (size_t k = 0; k < image.pixels.size(); ++k)
				for (int c = 0; c < 3; ++c)
				{
					const float f = std::clamp(static_cast<float>(mean[c]) / std::max(1.0f, blur[c][k]), 0.5f, 2.0f);
					image.pixels[k][c] = static_cast<uint8_t>(std::clamp(std::lround(image.pixels[k][c] * f), 0L, 255L));
				}
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
					// (planar, from the tile's middle: texture coordinates in the thousands lose their precision on the way to
					// the texture - research/phase0.md §61.25)
					const auto gu = [&](const P3& p) { return (p[0] - cx) / gravel.width; };
					const auto gv = [&](const P3& p) { return -(p[1] - cy) / gravel.height; };
					const uint32_t i0 = add(rm, a, n, t, gu(a), gv(a)), i1 = add(rm, b, n, t, gu(b), gv(b)), i2 = add(rm, c, n, t, gu(c), gv(c));
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
		// (how long each step took, for the log)
		auto lap = std::chrono::steady_clock::now();
		const auto took = [&](const char* step) {
			const auto now = std::chrono::steady_clock::now();
			std::printf("(%s: %.0f s)\n", step, std::chrono::duration<double>(now - lap).count());
			lap = now;
		};
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
		// (the coastline's water: the sea, before the map's lakes and rivers are drawn in - theirs are levels of their own)
		std::vector<uint8_t> seaCells(land.v.size());
		for (size_t k = 0; k < land.v.size(); ++k)
			seaCells[k] = land.v[k] == kWater;
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
		// the island: no land within `border` of the box's edges - those a neighbouring sector does not go on from (o.open)
		const auto toSea = [&](const P2& p) {
			float edge = std::numeric_limits<float>::max();
			if (o.open.find('w') == std::string::npos)
				edge = std::min(edge, p[0] - w.min[0]);
			if (o.open.find('e') == std::string::npos)
				edge = std::min(edge, w.max[0] - p[0]);
			if (o.open.find('s') == std::string::npos)
				edge = std::min(edge, p[1] - w.min[1]);
			if (o.open.find('n') == std::string::npos)
				edge = std::min(edge, w.max[1] - p[1]);
			return edge;
		};
		for (int j = 0; j < land.ny; ++j)
			for (int i = 0; i < land.nx; ++i)
			{
				const P2 c = land.Centre(i, j);
				const float edge = toSea(c);
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
		// grid's cells (1: water) reaching 8 m in over the shore so that waves still run up the beaches. land.json (written
		// with the map, below): that box and how calm the sea round the land is.
		std::array<float, 4> built{}; // (the terrain tiles' box: what is built)
		// (the mask, written with the water's levels once the heights are known)
		std::vector<uint8_t> maskBits;
		float maskX0 = 0, maskY0 = 0;
		uint32_t maskNx = 0, maskNy = 0;
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
			maskBits = std::move(bits);
			maskX0 = tx0, maskY0 = ty0, maskNx = nx, maskNy = ny;
			built = {tx0, ty0, tx1, ty1};
		}
		// the land cover (tools/worldgen/nlcd.py's landcover.bin, 'WBLC' version 1: x0, y0, cell (float), nx, ny (uint32),
		// then each cell's NLCD class, rows south to north): what the ground is where the map does not say
		raster::Grid<uint8_t> landCover;
		if (!o.landcover.empty())
		{
			std::ifstream in(o.landcover, std::ios::binary);
			char magic[4]{};
			uint32_t version = 0, nx = 0, ny = 0;
			in.read(magic, 4);
			in.read(reinterpret_cast<char*>(&version), 4);
			in.read(reinterpret_cast<char*>(&landCover.x0), 4);
			in.read(reinterpret_cast<char*>(&landCover.y0), 4);
			in.read(reinterpret_cast<char*>(&landCover.cell), 4);
			in.read(reinterpret_cast<char*>(&nx), 4);
			in.read(reinterpret_cast<char*>(&ny), 4);
			if (!in || std::memcmp(magic, "WBLC", 4) != 0 || version != 1 || landCover.cell <= 0 || !nx || !ny || nx > 65536 || ny > 65536)
				return error = "not a land cover file: " + o.landcover.string(), false;
			landCover.nx = static_cast<int>(nx), landCover.ny = static_cast<int>(ny);
			landCover.v.resize(static_cast<size_t>(nx) * ny);
			if (!in.read(reinterpret_cast<char*>(landCover.v.data()), static_cast<std::streamsize>(landCover.v.size())))
				return error = "a land cover file cut short: " + o.landcover.string(), false;
		}
		// (looked up through noise that bends its edges - 12 m over 50 m, 4 m over 17 m: NLCD's 30 m squares had every
		// edge between covers - lawns, fields, woods, the trees - in stair steps; they now wander as a field's or a wood's)
		const std::function<uint8_t(float, float)> landCoverAt = landCover.v.empty() ? std::function<uint8_t(float, float)>() : [&](float x, float y) {
			const float wx = x + 12.0f * Lattice(x, y, 50, 101) + 4.0f * Lattice(x, y, 17, 103), wy = y + 12.0f * Lattice(x, y, 50, 107) + 4.0f * Lattice(x, y, 17, 109);
			const auto c = landCover.Cell({wx, wy});
			return landCover.In(c[0], c[1]) ? landCover.At(c[0], c[1]) : uint8_t{0};
		};
		took("land and water");
		// 2. heights: land rises gently from the shore (a beach lower), the sea floor deepens away from it
		// (DistanceTo measures from the cells of a value to the nearest other cell)
		raster::Grid<uint8_t> dry = land, wet = land;
		for (size_t k = 0; k < land.v.size(); ++k)
			dry.v[k] = land.v[k] != kWater, wet.v[k] = land.v[k] == kWater;
		const auto toWater = raster::DistanceTo(dry, 1); // land cells: how far the water is
		const auto toLand = raster::DistanceTo(wet, 1);  // water cells: how far the land is
		// the measured ground (tools/worldgen/dem.py's ground.bin, 'WBGR' version 1: x0, y0, cell (float), nx, ny (uint32), then
		// each cell's height in centimetres (int16, -32768 not known), rows south to north): heights above NAVD88, about the
		// mean sea level here, the game's sea at 0
		raster::Grid<float> dem;
		if (!o.ground.empty())
		{
			std::ifstream in(o.ground, std::ios::binary);
			char magic[4]{};
			uint32_t version = 0, nx = 0, ny = 0;
			in.read(magic, 4);
			in.read(reinterpret_cast<char*>(&version), 4);
			in.read(reinterpret_cast<char*>(&dem.x0), 4);
			in.read(reinterpret_cast<char*>(&dem.y0), 4);
			in.read(reinterpret_cast<char*>(&dem.cell), 4);
			in.read(reinterpret_cast<char*>(&nx), 4);
			in.read(reinterpret_cast<char*>(&ny), 4);
			if (!in || std::memcmp(magic, "WBGR", 4) != 0 || version != 1 || dem.cell <= 0 || !nx || !ny || nx > 65536 || ny > 65536)
				return error = "not a ground file: " + o.ground.string(), false;
			std::vector<int16_t> cm(static_cast<size_t>(nx) * ny);
			if (!in.read(reinterpret_cast<char*>(cm.data()), static_cast<std::streamsize>(cm.size() * 2)))
				return error = "a ground file cut short: " + o.ground.string(), false;
			dem.nx = static_cast<int>(nx), dem.ny = static_cast<int>(ny);
			dem.v.resize(cm.size());
			for (size_t k = 0; k < cm.size(); ++k)
				dem.v[k] = cm[k] == -32768 ? std::numeric_limits<float>::quiet_NaN() : cm[k] / 100.0f;
		}
		// (bilinear; NaN where it is not known)
		const auto demAt = [&](float x, float y) {
			if (dem.v.empty())
				return std::numeric_limits<float>::quiet_NaN();
			const float fx = std::clamp((x - dem.x0) / dem.cell - 0.5f, 0.0f, dem.nx - 1.001f), fy = std::clamp((y - dem.y0) / dem.cell - 0.5f, 0.0f, dem.ny - 1.001f);
			const int i = static_cast<int>(fx), j = static_cast<int>(fy);
			const float tx = fx - i, ty = fy - j;
			const float a = dem.At(i, j), b = dem.At(i + 1, j), c = dem.At(i, j + 1), d = dem.At(i + 1, j + 1);
			if (std::isnan(a) || std::isnan(b) || std::isnan(c) || std::isnan(d))
			{
				const float nearest = tx < 0.5f ? (ty < 0.5f ? a : c) : (ty < 0.5f ? b : d);
				return nearest;
			}
			return (a * (1 - tx) + b * tx) * (1 - ty) + (c * (1 - tx) + d * tx) * ty;
		};
		// The water bodies (the land grid's water cells joined by their sides) and their levels: a body the coastline's
		// water is part of at the sea's (0); another at its ground's - the elevation is hydro-flattened: a lake's surface one
		// height, a river's stepping down along it - a body whose heights keep within 1.5 m all at their median, another
		// (a river) at its heights smoothed along it (60 m round each cell, within the body); in quarter metres. Without
		// measured ground all are the sea's.
		raster::Grid<float> waterLevel; // (NaN: land)
		waterLevel.x0 = land.x0, waterLevel.y0 = land.y0, waterLevel.cell = land.cell, waterLevel.nx = land.nx, waterLevel.ny = land.ny;
		waterLevel.v.assign(land.v.size(), std::numeric_limits<float>::quiet_NaN());
		std::vector<uint8_t> seaBody(land.v.size(), 0); // (a cell of a body at the sea's level)
		size_t lakes = 0, rivers = 0;
		{
			std::vector<int32_t> body(land.v.size(), -1);
			std::vector<int32_t> queue;
			int32_t bodies = 0;
			for (size_t start = 0; start < land.v.size(); ++start)
			{
				if (land.v[start] != kWater || body[start] >= 0)
					continue;
				queue.clear();
				queue.push_back(static_cast<int32_t>(start));
				body[start] = bodies;
				bool sea = false;
				for (size_t q = 0; q < queue.size(); ++q)
				{
					const int32_t k = queue[q];
					sea |= seaCells[k] != 0;
					const int i = k % land.nx, j = k / land.nx;
					const int next[4][2] = {{i - 1, j}, {i + 1, j}, {i, j - 1}, {i, j + 1}};
					for (const auto& n : next)
						if (n[0] >= 0 && n[1] >= 0 && n[0] < land.nx && n[1] < land.ny)
						{
							const int32_t kk = n[1] * land.nx + n[0];
							if (land.v[kk] == kWater && body[kk] < 0)
								body[kk] = bodies, queue.push_back(kk);
						}
				}
				++bodies;
				std::vector<float> heights;
				if (!sea && !dem.v.empty())
					for (const int32_t k : queue)
					{
						const P2 c = land.Centre(k % land.nx, k / land.nx);
						if (const float h = demAt(c[0], c[1]); !std::isnan(h))
							heights.push_back(h);
					}
				if (sea || heights.size() < queue.size() / 2 + 1)
				{
					for (const int32_t k : queue)
						waterLevel.v[k] = 0, seaBody[k] = 1;
					continue;
				}
				std::sort(heights.begin(), heights.end());
				const float p10 = heights[heights.size() / 10], p50 = heights[heights.size() / 2], p90 = heights[heights.size() * 9 / 10];
				const auto quarter = [](float h) { return std::max(0.0f, std::round(h * 4) / 4); };
				if (p90 - p10 <= 1.5f)
				{
					for (const int32_t k : queue)
						waterLevel.v[k] = quarter(p50);
					++lakes;
					continue;
				}
				// (a river: each cell its body's heights round it, by a sum table over the body's box)
				int i0 = land.nx, j0 = land.ny, i1 = 0, j1 = 0;
				for (const int32_t k : queue)
					i0 = std::min(i0, k % land.nx), i1 = std::max(i1, k % land.nx), j0 = std::min(j0, k / land.nx), j1 = std::max(j1, k / land.nx);
				const int bw = i1 - i0 + 1, bh = j1 - j0 + 1;
				std::vector<double> sum(static_cast<size_t>(bw + 1) * (bh + 1), 0.0), count(sum.size(), 0.0);
				for (int j = 0; j < bh; ++j)
					for (int i = 0; i < bw; ++i)
					{
						const int32_t k = (j0 + j) * land.nx + (i0 + i);
						double v = 0, n = 0;
						if (body[k] == bodies - 1)
						{
							const P2 c = land.Centre(i0 + i, j0 + j);
							if (const float h = demAt(c[0], c[1]); !std::isnan(h))
								v = h, n = 1;
						}
						const size_t at = static_cast<size_t>(j + 1) * (bw + 1) + (i + 1);
						sum[at] = v + sum[at - 1] + sum[at - (bw + 1)] - sum[at - (bw + 1) - 1];
						count[at] = n + count[at - 1] + count[at - (bw + 1)] - count[at - (bw + 1) - 1];
					}
				const int r = std::max(1, static_cast<int>(std::lround(60 / land.cell)));
				for (const int32_t k : queue)
				{
					const int i = k % land.nx - i0, j = k / land.nx - j0;
					const int a0 = std::max(0, i - r), a1 = std::min(bw, i + r + 1), b0 = std::max(0, j - r), b1 = std::min(bh, j + r + 1);
					const auto box = [&](const std::vector<double>& t) {
						return t[static_cast<size_t>(b1) * (bw + 1) + a1] - t[static_cast<size_t>(b0) * (bw + 1) + a1] - t[static_cast<size_t>(b1) * (bw + 1) + a0] +
						       t[static_cast<size_t>(b0) * (bw + 1) + a0];
					};
					const double n = box(count);
					waterLevel.v[k] = quarter(n > 0 ? static_cast<float>(box(sum) / n) : p50);
				}
				++rivers;
			}
			if (!dem.v.empty())
				std::printf("water: %d bodies, %zu lakes and %zu rivers above the sea's level\n", bodies, lakes, rivers);
		}
		const auto waterLevelAt = [&](float x, float y) {
			const auto c = waterLevel.Cell({x, y});
			return waterLevel.In(c[0], c[1]) ? waterLevel.At(c[0], c[1]) : std::numeric_limits<float>::quiet_NaN();
		};
		// (each land cell near water: the level of the water nearest it, 40 m in; the sea's or not)
		raster::Grid<float> shoreLevel = waterLevel;
		std::vector<uint8_t> shoreSea = seaBody;
		{
			std::vector<int32_t> queue;
			std::vector<uint8_t> steps(land.v.size(), 0);
			for (size_t k = 0; k < land.v.size(); ++k)
				if (!std::isnan(waterLevel.v[k]))
					queue.push_back(static_cast<int32_t>(k));
			const int reach = static_cast<int>(40 / land.cell);
			for (size_t q = 0; q < queue.size(); ++q)
			{
				const int32_t k = queue[q];
				if (steps[k] >= reach)
					continue;
				const int i = k % land.nx, j = k / land.nx;
				const int next[4][2] = {{i - 1, j}, {i + 1, j}, {i, j - 1}, {i, j + 1}};
				for (const auto& n : next)
					if (n[0] >= 0 && n[1] >= 0 && n[0] < land.nx && n[1] < land.ny)
					{
						const int32_t kk = n[1] * land.nx + n[0];
						if (std::isnan(shoreLevel.v[kk]))
							shoreLevel.v[kk] = shoreLevel.v[k], shoreSea[kk] = shoreSea[k], steps[kk] = steps[k] + 1, queue.push_back(kk);
					}
			}
		}
		// (the water up the land - a lake's, a river's - for their meshes: each water cell of a body over the sea's at its
		// level, the land 16 m round it at the nearest one's: the surface reaches under the banks, no gap at the shore)
		raster::Grid<float> upLand = waterLevel;
		for (size_t k = 0; k < upLand.v.size(); ++k)
		{
			if (seaBody[k])
				upLand.v[k] = std::numeric_limits<float>::quiet_NaN();
			else if (land.v[k] != kWater)
				upLand.v[k] = !shoreSea[k] && toWater.v[k] <= 16 ? shoreLevel.v[k] : std::numeric_limits<float>::quiet_NaN();
		}
		raster::Grid<float> height;
		height.x0 = land.x0, height.y0 = land.y0, height.cell = land.cell, height.nx = land.nx, height.ny = land.ny;
		height.v.resize(land.v.size());
		size_t measuredLand = 0;
		for (int j = 0; j < land.ny; ++j)
			for (int i = 0; i < land.nx; ++i)
			{
				const P2 c = land.Centre(i, j);
				const size_t at = static_cast<size_t>(j) * land.nx + i;
				const uint8_t k = land.At(i, j);
				float h;
				if (k != kWater)
				{
					const float d = toWater.At(i, j);
					const float measured = demAt(c[0], c[1]);
					if (!std::isnan(measured))
					{
						// the ground as measured; by water at least a little over its level, rising from it (by the sea
						// higher: its waves stay off the land but on beaches)
						h = measured, ++measuredLand;
						if (const float near = shoreLevel.v[at]; !std::isnan(near))
						{
							h = std::max(h, near + 0.25f + 0.04f * d);
							if (shoreSea[at] && k != kBeach)
								h = std::max(h, 0.6f + 0.8f * Smooth(d / 60));
						}
					}
					else
					{
						// (high enough that the sea's waves stay off it but on beaches, which they wash)
						h = 1.4f + 2.2f * Smooth(d / 150) + 0.4f * Noise(c[0], c[1]) * Smooth(d / 150);
						if (k == kBeach)
							h = std::min(h, 0.6f + 1.6f * Smooth(d / 60));
					}
				}
				else
				{
					// below its level, deepening from the shore (the sea further down off it; a lake or a river steeper - a
					// few metres deep a little way in, its murk then hides the bed)
					const float d = toLand.At(i, j);
					const float level = std::isnan(waterLevel.v[at]) ? 0.0f : waterLevel.v[at];
					h = seaBody[at] ? -(0.7f + 3.3f * (1 - std::exp(-d / 70)) + 22.0f * (1 - std::exp(-std::max(0.0f, d - 300) / 1500)))
					                : level - (1.0f + 4.0f * (1 - std::exp(-d / 25)));
				}
				height.At(i, j) = h;
			}
		if (!dem.v.empty())
			std::printf("ground: %zu land cells of %.0f m from the measured elevation\n", measuredLand, land.cell);
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
		// (the banks of the water up the land over its level again: the smoothing pulled the land by a lake down into it,
		// and the water then spread over the land to its mesh's edge, in 8 m steps)
		for (size_t k = 0; k < height.v.size(); ++k)
			if (land.v[k] != kWater && !shoreSea[k] && !std::isnan(shoreLevel.v[k]))
				height.v[k] = std::max(height.v[k], shoreLevel.v[k] + 0.1f + 0.04f * toWater.v[k]);
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
		// The land's water, for the loader (game/sea.cpp; next to the pack's dlc.rpf): 'WBWM' version 2: x0, y0, cell (float),
		// nx, ny (uint32), the sea's level (float); the bits (1: water), rows south to north; then its levels: how many (uint8)
		// and each (float, over the sea's); and a grid of 8 x 8 cells, each the index of the commonest level of its water
		// cells (uint8), ceil(nx / 8) x ceil(ny / 8), rows south to north.
		{
			const uint32_t lnx = (maskNx + 7) / 8, lny = (maskNy + 7) / 8;
			std::vector<float> levels{0.0f};
			std::vector<uint8_t> levelGrid(static_cast<size_t>(lnx) * lny, 0);
			for (uint32_t bj = 0; bj < lny; ++bj)
				for (uint32_t bi = 0; bi < lnx; ++bi)
				{
					std::map<float, int> seen;
					for (uint32_t j = bj * 8; j < std::min(maskNy, bj * 8 + 8); ++j)
						for (uint32_t i = bi * 8; i < std::min(maskNx, bi * 8 + 8); ++i)
						{
							const float lv = waterLevelAt(maskX0 + (i + 0.5f) * land.cell, maskY0 + (j + 0.5f) * land.cell);
							if (!std::isnan(lv))
								++seen[lv];
						}
					if (seen.empty())
						continue;
					const float lv = std::max_element(seen.begin(), seen.end(), [](const auto& a, const auto& b) { return a.second < b.second; })->first;
					auto it = std::find(levels.begin(), levels.end(), lv);
					if (it == levels.end())
					{
						if (levels.size() < 255)
							it = levels.insert(levels.end(), lv);
						else // (more than the index holds: the nearest kept)
							it = std::min_element(levels.begin(), levels.end(), [&](float a, float b) { return std::abs(a - lv) < std::abs(b - lv); });
					}
					levelGrid[static_cast<size_t>(bj) * lnx + bi] = static_cast<uint8_t>(it - levels.begin());
				}
			std::ofstream mask(o.out / "water.bin", std::ios::binary);
			const float sea = 0, cell = land.cell;
			const uint32_t version = 2;
			const uint8_t n = static_cast<uint8_t>(levels.size());
			mask.write("WBWM", 4);
			mask.write(reinterpret_cast<const char*>(&version), 4);
			mask.write(reinterpret_cast<const char*>(&maskX0), 4);
			mask.write(reinterpret_cast<const char*>(&maskY0), 4);
			mask.write(reinterpret_cast<const char*>(&cell), 4);
			mask.write(reinterpret_cast<const char*>(&maskNx), 4);
			mask.write(reinterpret_cast<const char*>(&maskNy), 4);
			mask.write(reinterpret_cast<const char*>(&sea), 4);
			mask.write(reinterpret_cast<const char*>(maskBits.data()), static_cast<std::streamsize>(maskBits.size()));
			mask.write(reinterpret_cast<const char*>(&n), 1);
			mask.write(reinterpret_cast<const char*>(levels.data()), static_cast<std::streamsize>(levels.size() * sizeof(float)));
			mask.write(reinterpret_cast<const char*>(levelGrid.data()), static_cast<std::streamsize>(levelGrid.size()));
			if (levels.size() > 1)
				std::printf("water: %zu levels above the sea's (up to %.2f m)\n", levels.size() - 1, *std::max_element(levels.begin(), levels.end()));
		}
		const auto ground = [&](float x, float y) {
			const float fx = std::clamp((x - height.x0) / height.cell - 0.5f, 0.0f, height.nx - 1.001f),
			            fy = std::clamp((y - height.y0) / height.cell - 0.5f, 0.0f, height.ny - 1.001f);
			const int i = static_cast<int>(fx), j = static_cast<int>(fy);
			const float tx = fx - i, ty = fy - j;
			const float a = height.At(i, j), b = height.At(i + 1, j), c = height.At(i, j + 1), d = height.At(i + 1, j + 1);
			return (a * (1 - tx) + b * tx) * (1 - ty) + (c * (1 - tx) + d * tx) * ty;
		};

		took("heights");
		// what covers the ground (research/phase0.md §61.25): lawn, paving, sand or bare earth, from the map's classes and,
		// where there is an aerial picture, what it shows - its near infrared (vegetation: NDVI 0.12 and more, or green
		// to the eye: artificial turf) in built-up land, parks and the rest; on land, lawns from 14 m off the water
		enum : uint8_t { kLawn, kPaving, kBare, kSandy, kUnder };
		raster::Grid<uint8_t> cover = land, lawnCapable = land, forestCapable = land;
		{
			elevation::Aerial photo;
			const bool photographed = !o.aerial.empty() && photo.Open(o.aerial);
			size_t counts[5]{};
			for (int j = 0; j < land.ny; ++j)
				for (int i = 0; i < land.nx; ++i)
				{
					const uint8_t k = land.At(i, j);
					const P2 c = land.Centre(i, j);
					const float h = height.At(i, j);
					uint8_t what = kLawn;
					if (k == kWater || h < 0.2f)
						what = kUnder;
					else if (k == kBeach)
						what = kSandy;
					else if (k == kPaved)
						what = kPaving;
					else if (landCoverAt && !photographed)
					{
						// (fields ploughed, broadleaf woods' and swamps' floors bare; barren land sand; pine flatwoods, grassland,
						// pastures and yards grassy)
						const uint8_t lc = landCoverAt(c[0], c[1]);
						what = lc == 82 || lc == 41 || lc == 90 ? kBare : lc == 31 ? kSandy : kLawn;
					}
					else if (photographed)
					{
						float ndvi = 0, green = 0, brown = 0;
						int n = 0;
						for (int s = 0; s < 9; ++s)
						{
							std::array<float, 4> px{};
							if (!photo.Sample(c[0] + (s % 3 - 1) * land.cell / 3, c[1] + (s / 3 - 1) * land.cell / 3, px))
								continue;
							ndvi += (px[3] - px[0]) / std::max(1.0f, px[3] + px[0]);
							green += px[1] - (px[0] + px[2]) / 2;
							brown += px[0] - px[2];
							++n;
						}
						if (n)
						{
							ndvi /= n, green /= n, brown /= n;
							what = ndvi >= 0.12f || green > 8 ? kLawn : brown > 15 ? kBare : kPaving;
						}
					}
					cover.At(i, j) = what;
					++counts[what];
					lawnCapable.At(i, j) = what == kLawn && toWater.At(i, j) >= 14;
					forestCapable.At(i, j) = what == kBare && toWater.At(i, j) >= 6;
				}
			// (the picture's specks smoothed: on land each cell takes what most of the nine round it are - lawn, paving or
			// bare earth)
			if (photographed)
			{
				const auto before = cover.v;
				for (int j = 1; j + 1 < land.ny; ++j)
					for (int i = 1; i + 1 < land.nx; ++i)
					{
						const uint8_t self = before[static_cast<size_t>(j) * land.nx + i];
						if (self > kBare)
							continue;
						int votes[3]{};
						for (int dj = -1; dj <= 1; ++dj)
							for (int di = -1; di <= 1; ++di)
								if (const uint8_t c = before[static_cast<size_t>(j + dj) * land.nx + i + di]; c <= kBare)
									++votes[c];
						const uint8_t best = static_cast<uint8_t>(std::max_element(votes, votes + 3) - votes);
						if (votes[best] >= 5 && best != self)
						{
							cover.At(i, j) = best;
							lawnCapable.At(i, j) = best == kLawn && toWater.At(i, j) >= 14;
						}
					}
			}
			std::printf("ground cover: %zu lawn, %zu paving, %zu bare, %zu sand, %zu under water (4 m cells)%s\n", counts[0], counts[1], counts[2], counts[3],
			    counts[4], photographed ? "" : " (no aerial pictures: built-up land as lawn)");
		}
		// (how far into a lawn: from its edge)
		const auto lawnDepth = raster::DistanceTo(lawnCapable, 1);
		const auto forestDepth = raster::DistanceTo(forestCapable, 1);
		const auto forestDepthAt = [&](float x, float y) {
			const auto c = forestDepth.Cell({x, y});
			return forestDepth.In(c[0], c[1]) ? forestDepth.At(c[0], c[1]) : 0.0f;
		};
		const auto coverAt = [&](float x, float y) {
			const auto c = cover.Cell({x, y});
			return cover.In(c[0], c[1]) ? cover.At(c[0], c[1]) : uint8_t{kUnder};
		};
		const auto depthAt = [&](float x, float y) {
			const auto c = lawnDepth.Cell({x, y});
			return lawnDepth.In(c[0], c[1]) ? lawnDepth.At(c[0], c[1]) : 0.0f;
		};

		took("ground cover");
		// 3. roads (those reaching into the grids), cut where they run into the sea round the island (its border, above:
		// they went on through it, the trees beside them standing in the water)
		world::World near = w;
		std::erase_if(near.roads, [&](const world::Road& r) {
			return std::none_of(r.points.begin(), r.points.end(), [&](const P2& p) { return p[0] >= gx0 && p[0] <= gx1 && p[1] >= gy0 && p[1] <= gy1; });
		});
		const auto inBorderSea = [&](const P2& p) {
			// (and past the box itself: at an open edge the road stops there, the next sector's goes on)
			return toSea(p) < o.border + 60 * Noise(p[0] * 3, p[1] * 3) + 40 || p[0] < w.min[0] || p[0] > w.max[0] || p[1] < w.min[1] || p[1] > w.max[1];
		};
		std::set<std::pair<float, float>> cutEnds; // (where a road now ends because of the cut)
		size_t cuts = 0;
		{
			std::vector<world::Road> kept;
			for (const auto& r : near.roads)
			{
				world::Road piece = r;
				piece.points.clear();
				for (size_t k = 0; k <= r.points.size(); ++k)
				{
					if (k < r.points.size() && !inBorderSea(r.points[k]))
					{
						if (piece.points.empty() && k > 0)
							cutEnds.insert({r.points[k][0], r.points[k][1]});
						piece.points.push_back(r.points[k]);
						continue;
					}
					if (k < r.points.size())
						++cuts;
					if (!piece.points.empty() && k < r.points.size())
						cutEnds.insert({piece.points.back()[0], piece.points.back()[1]});
					if (piece.points.size() >= 2)
						kept.push_back(piece);
					piece.points.clear();
				}
			}
			near.roads = std::move(kept);
		}
		roads::Network network = roads::Build(near);
		{
			// (the stubs the cut left pruned back to their last junction: a road on into the sea would be a dead end the
			// traffic drives into)
			std::vector<bool> gone(network.edges.size(), false);
			std::vector<int> degree(network.vertices.size());
			for (size_t v = 0; v < network.vertices.size(); ++v)
				degree[v] = static_cast<int>(network.vertices[v].edges.size());
			size_t pruned = 0;
			for (size_t v0 = 0; v0 < network.vertices.size(); ++v0)
			{
				if (degree[v0] != 1 || !cutEnds.contains({network.vertices[v0].p[0], network.vertices[v0].p[1]}))
					continue;
				for (int v = static_cast<int>(v0); degree[v] == 1;)
				{
					const auto& list = network.vertices[v].edges;
					const auto e = std::find_if(list.begin(), list.end(), [&](int x) { return !gone[x]; });
					if (e == list.end())
						break;
					gone[*e] = true;
					++pruned;
					const roads::Edge& ed = network.edges[*e];
					--degree[ed.a], --degree[ed.b];
					v = ed.a == v ? ed.b : ed.a;
				}
			}
			std::vector<int> index(network.edges.size(), -1);
			std::vector<roads::Edge> edges;
			for (size_t e = 0; e < network.edges.size(); ++e)
				if (!gone[e])
				{
					index[e] = static_cast<int>(edges.size());
					edges.push_back(std::move(network.edges[e]));
				}
			network.edges = std::move(edges);
			for (auto& vx : network.vertices)
			{
				std::vector<int> list;
				for (const int x : vx.edges)
					if (index[x] >= 0)
						list.push_back(index[x]);
				vx.edges = std::move(list);
			}
			std::printf("roads: %zu points in the sea round the island cut, %zu road pieces to them pruned\n", cuts, pruned);
		}
		// (over water - a stream the map has no bridge over - a road at least a little above its level)
		roads::Heights(network, [&](float x, float y) {
			const float g = ground(x, y), lv = waterLevelAt(x, y);
			return std::isnan(lv) ? g : std::max(g, lv + 0.4f);
		});
		roads::Surfaces surfaces;
		{
			// (a divided road's median: planted where the aerial picture shows vegetation, as the ground cover - but on the
			// picture itself, the cover's 4 m cells being as wide as the median)
			elevation::Aerial photo;
			std::function<bool(float, float)> planted;
			if (!o.aerial.empty() && photo.Open(o.aerial))
				planted = [&](float x, float y) {
					int green = 0, n = 0;
					for (int k = 0; k < 5; ++k)
					{
						std::array<float, 4> px{};
						if (!photo.Sample(x + (k == 1 ? 0.6f : k == 2 ? -0.6f : 0.0f), y + (k == 3 ? 0.6f : k == 4 ? -0.6f : 0.0f), px))
							continue;
						const float ndvi = (px[3] - px[0]) / std::max(1.0f, px[3] + px[0]);
						green += ndvi >= 0.12f || px[1] - (px[0] + px[2]) / 2 > 8;
						++n;
					}
					return n > 0 && green * 2 > n;
				};
			roads::Geometry(network, surfaces, planted);
		}
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

		took("roads");
		// buildings: with a scan, of our own (each footprint's measured shape, walls in a style picked by its kind and
		// size); without, the game's models on the footprints (the ground under each no higher than its floor)
		std::vector<structures::Shape> shapes;
		std::vector<int> shapeStyle;
		std::vector<P2> shapeCentre;
		facades::Set facadeSet;
		std::vector<world::Building> found; // (the buildings the scan saw that the map lacks)
		std::vector<std::vector<P2>> mapped; // (the footprints of those built, for the game's map)
		if (o.buildings && scanned)
		{
			found = structures::Missing([&](elevation::Scan& s) { s.Open(o.scan, o.datum); }, w.buildings, {bx0, by0, bx1, by1});
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
			// (a scan tile at a time: in the map's order the buildings jump about the city and the scan's tiles, read as they
			// are asked for, were read again and again - 96 GB for the whole of Miami)
			const auto scanTile = [](const world::Building* b) {
				return b->footprint.empty() ? std::make_pair(0, 0)
				                            : std::make_pair(static_cast<int>(std::floor(b->footprint[0][1] / 256)), static_cast<int>(std::floor(b->footprint[0][0] / 256)));
			};
			std::stable_sort(all.begin(), all.end(), [&](const world::Building* a, const world::Building* b) { return scanTile(a) < scanTile(b); });
			// (on all the cores: each worker takes runs of the sorted buildings - their scan tiles together - with a scan
			// reader of its own; the shapes kept in the buildings' order)
			struct Made
			{
				bool ok = false;
				structures::Shape s;
				P2 c{};
			};
			std::vector<Made> made(all.size());
			const unsigned workerCount = std::max(1u, std::thread::hardware_concurrency());
			std::vector<structures::Stats> partStats(workerCount);
			{
				constexpr size_t kRun = 64;
				std::atomic<size_t> next = 0;
				std::vector<std::thread> workers;
				for (unsigned wk = 0; wk < workerCount; ++wk)
					workers.emplace_back([&, wk] {
						elevation::Scan own;
						own.Open(o.scan, o.datum);
						for (size_t first; (first = next.fetch_add(kRun)) < all.size();)
							for (size_t i = first; i < std::min(all.size(), first + kRun); ++i)
							{
								const world::Building& b = *all[i];
								if (b.footprint.size() < 3)
									continue;
								P2 c{0, 0};
								for (const P2& q : b.footprint)
									c[0] += q[0] / b.footprint.size(), c[1] += q[1] / b.footprint.size();
								if (c[0] < bx0 || c[0] >= bx1 || c[1] < by0 || c[1] >= by1 || ground(c[0], c[1]) < 0.3f)
									continue;
								made[i].ok = structures::Make(b, &own, ground, so, made[i].s, partStats[wk]);
								made[i].c = c;
							}
					});
				for (auto& worker : workers)
					worker.join();
			}
			for (const auto& p : partStats)
			{
				ss.measured += p.measured, ss.flat += p.flat, ss.shaped += p.shaped, ss.mapHeight += p.mapHeight, ss.skipped += p.skipped;
				ss.triangles += p.triangles;
			}
			for (size_t i = 0; i < all.size(); ++i)
			{
				if (!made[i].ok)
					continue;
				const world::Building& b = *all[i];
				const P2 c = made[i].c;
				mapped.push_back(b.footprint);
				const float area = static_cast<float>(std::fabs(polygon::Area(b.footprint)));
				const uint32_t seed = static_cast<uint32_t>(std::lround(c[0] * 3)) * 73856093u ^ static_cast<uint32_t>(std::lround(c[1] * 3)) * 19349663u;
				shapeStyle.push_back(facades::Pick(facadeSet, b.kind, made[i].s.height, area, seed));
				shapeCentre.push_back(c);
				shapes.push_back(std::move(made[i].s));
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
				mapped.push_back(b.footprint);
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

		took("buildings");
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
				const auto lc = land.Cell({x, y});
				// (and on land: none in the water - the sea round the island, the bay, the canals)
				return blocked.In(c[0], c[1]) && !blocked.At(c[0], c[1]) && land.In(lc[0], lc[1]) && land.At(lc[0], lc[1]) != kWater;
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
			else
			{
				// (no scan: trees by the land cover, else the map's woods and wetlands)
				trees::Stats ts;
				trees::Fill(w.areas, landCoverAt, w.projection.lat0, open, ground, {bx0, by0, bx1, by1}, furniture, ts);
				std::printf("trees in woods and wetlands: %zu palms, %zu broadleaf and pines, %zu shrubs\n", ts.palms, ts.broadleaf, ts.bushes);
			}
			{
				// nothing in the cars' way: trees and posts off the carriageways (the roads' and the junctions' asphalt, as their
				// collision has it - the grid above only has the roads' widths along their lines, in 2 m cells) by their
				// clearances, more by the street junctions' corners, where turning cars cut across the kerb (a signal mast by
				// a corner's kerb held a turning car, and the junction's traffic behind it, for good)
				const roads::Asphalt asphalt(surfaces.collision, roads::kCornerTreeClear);
				constexpr float kCell = 8;
				const auto key = [](int i, int j) { return static_cast<int64_t>(i) << 32 | static_cast<uint32_t>(j); };
				const auto cellOf = [](float v) { return static_cast<int>(std::floor(v / kCell)); };
				// (the street junctions' corners)
				std::unordered_map<int64_t, std::vector<std::pair<P2, float>>> corners;
				for (size_t vi = 0; vi < network.vertices.size(); ++vi)
				{
					const auto& v = network.vertices[vi];
					const float reach = roads::CornerReach(network, static_cast<int>(vi));
					if (reach <= 0)
						continue;
					for (int j = cellOf(v.p[1] - reach); j <= cellOf(v.p[1] + reach); ++j)
						for (int i = cellOf(v.p[0] - reach); i <= cellOf(v.p[0] + reach); ++i)
							corners[key(i, j)].push_back({v.p, reach});
				}
				const auto atCorner = [&](float x, float y) {
					const auto cell = corners.find(key(cellOf(x), cellOf(y)));
					return cell != corners.end() && std::any_of(cell->second.begin(), cell->second.end(), [&](const std::pair<P2, float>& c) {
						return std::hypot(c.first[0] - x, c.first[1] - y) < c.second;
					});
				};
				const uint32_t posts[] = {loader::convert::Joaat("prop_streetlight_01"), loader::convert::Joaat("prop_traffic_01a"),
				    loader::convert::Joaat("prop_traffic_01b")};
				size_t trees = 0, postsOut = 0;
				std::erase_if(furniture, [&](const props::Prop& p) {
					const bool post = std::find(std::begin(posts), std::end(posts), p.archetype) != std::end(posts);
					const bool corner = atCorner(p.position[0], p.position[1]);
					const float r = post ? (corner ? roads::kCornerPostClear : roads::kPostClear) : (corner ? roads::kCornerTreeClear : roads::kTreeClear);
					if (!asphalt.Within(p.position[0], p.position[1], r))
						return false;
					++(post ? postsOut : trees);
					return true;
				});
				std::printf("in the cars' way, left out: %zu trees, %zu posts\n", trees, postsOut);
			}
			{
				// (what stands where, for checking: archetype hash, x, y, z, scale)
				std::ofstream list(o.out.parent_path() / (o.name + "_props.txt"));
				for (const auto& prop : furniture)
					list << std::format("{:08x} {:.1f} {:.1f} {:.1f} {:.2f}\n", prop.archetype, prop.position[0], prop.position[1], prop.position[2], prop.scale);
			}
		}

		took("props and trees");
		// the game's map of the land (research/phase0.md §61.26): the radar's tiles and the pause map's pictures; land.json
		// with what the loader needs of them
		{
			minimap::Input mi;
			mi.box = built;
			mi.height = &height;
			mi.buildings = mapped;
			for (const auto& e : network.edges)
			{
				minimap::Road r;
				r.points = e.points;
				r.halfWidth = e.HalfWidth();
				const std::string kind = e.kind.ends_with("_link") ? e.kind.substr(0, e.kind.size() - 5) : e.kind;
				r.grey = kind == "service" || kind == "pedestrian" ? 130 : kind == "track" || kind == "path" || kind == "footway" || kind == "cycleway" ? 99 : 180;
				mi.roads.push_back(std::move(r));
			}
			minimap::Output mo;
			if (!minimap::Write(files, mi, o.name, o.out, mo, error))
				return error = "map: " + error, false;
			std::string radar, pictures;
			for (const auto& [c, r] : mo.supertiles)
				radar += std::format("{}[{}, {}]", radar.empty() ? "" : ", ", c, r);
			for (const auto& [r, c] : mo.pictures)
				pictures += std::format("{}[{}, {}]", pictures.empty() ? "" : ", ", r, c);
			std::printf("map: %zu radar supertiles (%zu triangles), %zu pause map pictures\n", mo.supertiles.size(), mo.triangles, mo.pictures.size());
			// (and for the loader's sectors: where the game's coordinates are in the real world - x = x0 + (lon - lon0) kx,
			// y = y0 + (lat - lat0) ky - the land's own box, and the edges a neighbouring sector goes on from)
			const auto& pr = w.projection;
			std::ofstream(o.out / "land.json") << std::format(
			    "{{\"box\": [{:.0f}, {:.0f}, {:.0f}, {:.0f}], \"waves\": {:.2f}, \"water\": \"water.bin\", \"climate\": \"{}\", \"map\": {{\"name\": \"{}\", "
			    "\"radar\": [{}], \"pictures\": [{}]}}, \"projection\": [{:.9f}, {:.9f}, {:.3f}, {:.3f}, {:.6f}, {:.6f}], \"world\": [{:.1f}, {:.1f}, "
			    "{:.1f}, {:.1f}], \"open\": \"{}\"}}\n",
			    built[0], built[1], built[2], built[3], o.waves, o.climate.empty() ? w.climate : o.climate, o.name, radar, pictures, pr.lat0, pr.lon0, pr.x0, pr.y0, pr.kx, pr.ky,
			    w.min[0], w.min[1], w.max[0], w.max[1], o.open);
		}

		took("map");
		// 4. tiles: terrain and roads, their collision; entities and archetypes
		ydr::Material ground3, asphalt, sidewalk, kerb, marking;
		// (terrain_cb_4lyr: the terrain shader without the rain's puddles, which on flat ground mirror the sky in blue patches)
		if (!GameMaterial(files, "kt1_04_02_r3iint.ydr", 4, ground3, error) || !GameMaterial(files, "dt1_rd1_r1_05.ydr", 1, asphalt, error) ||
		    !GameMaterial(files, "dt1_rd1_r1_05.ydr", 3, sidewalk, error) || !GameMaterial(files, "dt1_rd1_r1_05.ydr", 0, kerb, error) ||
		    !GameMaterial(files, "dt1_rd1_r1_05.ydr", 5, marking, error))
			return false;
		// the lawns' (terrain_cb_w_4lyr_2tex_blend: the game's own lawns', a Rockford Hills ground model's; the only
		// blending terrain shader the game has is this one with the rain's puddles - after rain they mirrored the sky over
		// half a lawn: grass holds little standing water, so its layers' materialWetnessMultiplier (the game's 0.2-0.35)
		// down to a tenth of that)
		ydr::Material lawnMaterial, groundForest, riverMaterial, forestMaterial;
		if (!GameMaterial(files, "bh1_21_grnd02_1.ydr", 2, lawnMaterial, error))
			return false;
		for (auto& [parameter, bytes] : lawnMaterial.constants)
			if (parameter == loader::convert::Joaat("materialWetnessMultiplier"))
				for (size_t k = 0; k + 4 <= bytes.size(); k += 4)
				{
					float v;
					std::memcpy(&v, bytes.data() + k, 4);
					v *= 0.1f;
					std::memcpy(bytes.data() + k, &v, 4);
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
			// (flat: a ground's texture, Flatten'ed - in the format it had)
			const auto use = [&](const char* dictionary, const char* texture, std::string& name, bool flat = false) {
				const auto key = std::make_pair(std::string(dictionary), std::string(texture) + (flat ? "#flat" : ""));
				if (const auto it = copied.find(key); it != copied.end())
					return name = it->second, true;
				loader::convert::DictionaryTexture t;
				name = std::format("{}_{}{}", o.name, texture, flat ? "_flat" : "");
				if (!CopyTexture(files, dictionary, texture, name, t, error))
					return false;
				if (flat)
				{
					texture::Image image;
					if (!GameImage(files, dictionary, texture, image, error))
						return false;
					Flatten(image);
					t = texture::Make(name, image, t.format.dxgi == 77 ? texture::Format::Bc3 : texture::Format::Bc1);
				}
				shared.push_back(std::move(t));
				copied[key] = name;
				return true;
			};
			// the terrain's (research/phase0.md §61.25): the ground material's layers - the grass the lawns' layer 3 is too
			// (where the two materials meet both draw it), paving, sand, wet sand - and the lawns' four grasses and their
			// blend map
			{
				const std::array<std::array<const char*, 4>, 4> groundLayers{{{"ch1_06e_land+hi.ytd", "im_grass10b", "ch1_09_arc007+hidr.ytd", "im_grass07_n"},
				    {"sp1_03_ground+hidr.ytd", "im_concrete25", "paletoroadtemp+hi.ytd", "im_concrete25_n"},
				    {"venicebeachground+hi.ytd", "nxg_rsn_os_beachsoftprints", "venicebeachground+hi.ytd", "nxg_rsn_os_beachsoftprints_n"},
				    {"prop_beach+hi.ytd", "rsn_os_beachhardwater", "sm_22_pier+hidr.ytd", "rsn_os_beachhardwater_n"}}};
				const std::array<std::array<const char*, 2>, 4> lawns{{{"bh1_05_ground1+hidr.ytd", "im_grass11b"}, {"bh1_15_alleygr+hidr.ytd", "im_grass07b"},
				    {"ch1_06e_land+hi.ytd", "im_grass014b"}, {"ch1_06e_land+hi.ytd", "im_grass10b"}}};
				std::string d, n;
				for (int k = 0; k < 4; ++k)
				{
					if (!use(groundLayers[k][0], groundLayers[k][1], d, true) || !use(groundLayers[k][2], groundLayers[k][3], n))
						return false;
					ground3.Texture(loader::convert::Joaat(std::format("DiffuseTexture_layer{}", k)), d);
					ground3.Texture(loader::convert::Joaat(std::format("BumpTexture_layer{}", k)), n);
					if (!use(lawns[k][0], lawns[k][1], d, true) || !use("ch1_09_arc007+hidr.ytd", "im_grass07_n", n))
						return false;
					lawnMaterial.Texture(loader::convert::Joaat(std::format("DiffuseTexture_layer{}", k)), d);
					lawnMaterial.Texture(loader::convert::Joaat(std::format("BumpTexture_layer{}", k)), n);
				}
				// The lawns' blend map: one of our own, 128 m a side (the game's repeats every 22 m - a grid seen from a little
				// way off): value noise at 1.5 to 40 m, repeating with the map. Its green the second pair's share (their
				// patchy grass in patches, a quarter of the lawn), blue the layer in each pair, red a little darkening.
				{
					constexpr int kSide = 512;
					texture::Image blend(kSide, kSide);
					const auto octaves = [&](float x, float y, uint32_t seed) {
						float v = 0, total = 0, amplitude = 1;
						for (int k = 0, cell = 128; cell >= 6; ++k, cell /= 2, amplitude *= 0.55f)
							v += amplitude * Lattice(x, y, static_cast<float>(cell), seed + k, kSide / cell), total += amplitude;
						return v / total;
					};
					for (int y = 0; y < kSide; ++y)
						for (int x = 0; x < kSide; ++x)
						{
							const float g = Smooth((octaves(static_cast<float>(x), static_cast<float>(y), 11) - 0.15f) / 0.5f);
							const float b = std::clamp(0.5f + 0.9f * octaves(static_cast<float>(x), static_cast<float>(y), 29), 0.0f, 1.0f);
							const float r = std::clamp(0.06f + 0.12f * octaves(static_cast<float>(x), static_cast<float>(y), 47), 0.0f, 0.2f);
							blend.At(x, y) = {static_cast<uint8_t>(r * 255), static_cast<uint8_t>(g * 255), static_cast<uint8_t>(b * 255), 255};
						}
					shared.push_back(texture::Make(o.name + "_grassblend", blend, texture::Format::Bc1));
					lawnMaterial.Texture(loader::convert::Joaat("lookupTexture"), o.name + "_grassblend");
				}
				// The forest floors' (bare earth inland: woods, swamps, fields) - the lawns' shader and blend map with two leaf
				// litters, grass and earth, its layer 3 the ground material's leaf litter (where the two meet)
				forestMaterial = lawnMaterial;
				{
					// (each pair a litter and a grass - the woods' floors grassy in places, as Florida's pine flatwoods are - so
					// that the blend map's mix never leaves one litter's repeat on its own)
					const std::array<std::array<const char*, 4>, 4> litter{{{"bh1_36_terrain+hi.ytd", "rsn_am_ground_dirt_leaflitter", "bh1_36_terrain+hi.ytd",
					                                                           "rsn_am_ground_dirt_leaflitter_n"},
					    {"bh1_15_alleygr+hidr.ytd", "im_grass07b", "ch1_09_arc007+hidr.ytd", "im_grass07_n"},
					    {"bh1_43.ytd", "rsn_am_ground_dirt_dry_leaflitter", "bh1_36_terrain+hi.ytd", "rsn_am_ground_dirt_leaflitter_n"},
					    {"bh1_36_terrain+hi.ytd", "rsn_am_ground_dirt_leaflitter", "bh1_36_terrain+hi.ytd", "rsn_am_ground_dirt_leaflitter_n"}}};
					for (int k = 0; k < 4; ++k)
					{
						if (!use(litter[k][0], litter[k][1], d, true) || !use(litter[k][2], litter[k][3], n))
							return false;
						forestMaterial.Texture(loader::convert::Joaat(std::format("DiffuseTexture_layer{}", k)), d);
						forestMaterial.Texture(loader::convert::Joaat(std::format("BumpTexture_layer{}", k)), n);
					}
				}
				// the forest floor (inland tiles: leaf litter in place of the beach's sand, mud in place of the wet sand)
				groundForest = ground3;
				if (!use("bh1_36_terrain+hi.ytd", "rsn_am_ground_dirt_leaflitter", d, true) || !use("bh1_36_terrain+hi.ytd", "rsn_am_ground_dirt_leaflitter_n", n))
					return false;
				groundForest.Texture(loader::convert::Joaat("DiffuseTexture_layer2"), d);
				groundForest.Texture(loader::convert::Joaat("BumpTexture_layer2"), n);
				// (and under the lakes and rivers a mud bank's mud, not the beach's wet sand: through the water it had shown as a
				// bright sandy floor)
				if (!use("ch1_09_terrain+hi.ytd", "rn_tf_mudbank_003", d, true) || !use("ch1_09_terrain+hi.ytd", "rn_tf_mudbank_002_n", n))
					return false;
				groundForest.Texture(loader::convert::Joaat("DiffuseTexture_layer3"), d);
				groundForest.Texture(loader::convert::Joaat("BumpTexture_layer3"), n);
				// the water up the land: the game's river water (water_river: a golf course pond's, still) with a brown creek's
				// fog (one colour, 4 x 4 - the pond's is a picture laid over its own texture coordinates): Florida's lakes,
				// ponds and rivers are mostly tea-coloured with tannin, black from above (the aerial pictures); the country
				// creek's dark green fog had them blue and clear over a pale floor
				if (!GameMaterial(files, "bh1_43_water004.ydr", 0, riverMaterial, error) || !use("bh1_43_water.ytd", "flow_blank", d) ||
				    !use("ch3_12_water.ytd", "ch3_12_fog", n))
					return false;
				riverMaterial.Texture(0x8FC59620, d); // (its flow map)
				riverMaterial.Texture(0x85261019, n); // (its fog)
			}
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
		// (what lies in which tile, looked up once: each tile going through all of them was most of a city's build)
		const auto index = [&]() { return TileIndex(w.min[0], w.min[1], o.tile, ti0, tj0, ti1, tj1); };
		TileIndex collisionTiles = index(), placedTiles = index(), shapeTiles = index(), furnitureTiles = index();
		for (size_t t = 0; t < surfaces.collision.triangles.size(); ++t)
		{
			const auto& tri = surfaces.collision.triangles[t];
			const P3 &a = surfaces.collision.vertices[tri[0]], &b = surfaces.collision.vertices[tri[1]], &c = surfaces.collision.vertices[tri[2]];
			collisionTiles.Add(static_cast<uint32_t>(t), (a[0] + b[0] + c[0]) / 3, (a[1] + b[1] + c[1]) / 3);
		}
		for (size_t k = 0; k < placed.size(); ++k)
			placedTiles.Add(static_cast<uint32_t>(k), placed[k].centre[0], placed[k].centre[1]);
		for (size_t k = 0; k < shapes.size(); ++k)
			shapeTiles.Add(static_cast<uint32_t>(k), shapeCentre[k][0], shapeCentre[k][1]);
		for (size_t k = 0; k < furniture.size(); ++k)
			furnitureTiles.Add(static_cast<uint32_t>(k), furniture[k].position[0], furniture[k].position[1]);
		std::map<const roads::Mesh*, TileIndex> meshTiles;
		for (const roads::Mesh* m : {&surfaces.worn, &surfaces.sidewalk, &surfaces.kerb, &surfaces.patches, &surfaces.potholes, &surfaces.breakups, &surfaces.markings})
			meshTiles.emplace(m, MeshTiles(*m, w.min[0], w.min[1], o.tile, ti0, tj0, ti1, tj1));
		for (const auto& m : surfaces.paved)
			meshTiles.emplace(&m, MeshTiles(m, w.min[0], w.min[1], o.tile, ti0, tj0, ti1, tj1));
		// (a tile: what it adds to the maps, the archetypes, the navmesh and the counts. The tiles are made on all the cores,
		// each worker with its own aerial picture reader; what they add is put together in the tiles' order after, as one
		// going through them in turn had it)
		struct TileResult
		{
			std::pair<int, int> block{};
			std::vector<mapfiles::Archetype> archetypes;
			std::vector<mapfiles::Entity> entities; // (the block's, in order)
			bool hasLod = false;
			mapfiles::Entity lod; // (the LOD map's entity of entities[lodOwner])
			size_t lodOwner = 0;
			std::vector<std::pair<std::string, std::string>> parents;
			bounds::Mesh nav;
			std::vector<navgen::Surface> navSurfaces;
			size_t terrainTriangles = 0, roadTriangles = 0, generatedTiles = 0, tiles = 0;
		};
		const auto makeTile = [&](int ti, int tj, elevation::Aerial& aerial, TileResult& r, std::string& error) -> bool {
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
					// the ground material's layers: grass (the lawn's layer 3), paving, sand (beaches, bare earth, the
					// shore; inland the forest floor's leaf litter), wet sand (under water) - each vertex the share of five
					// samples round it (their places jittered by noise: the edges between covers soft and irregular, not
					// the mesh's grid)
					float grass = 0, paving = 0, soft = 0, wetSand = 0;
					const float lw = h < 0.2f ? 1.0f : 0.0f;
					for (int sample = 0; sample < 5; ++sample)
					{
						const float a = sample * 1.2566f + Lattice(x, y, 23, 5) * 3.0f, rr = sample ? o.cell * (0.45f + 0.35f * Lattice(x, y, 17, 9 + sample)) : 0;
						const float sx = x + rr * std::cos(a) + 3.0f * Lattice(x, y, 11, 13), sy = y + rr * std::sin(a) + 3.0f * Lattice(x, y, 13, 17);
						const uint8_t what = lw > 0 ? uint8_t{kUnder} : coverAt(sx, sy);
						if (what == kUnder)
							wetSand += 0.2f;
						else if (what == kSandy || what == kBare)
							soft += 0.2f;
						else if (what == kPaving)
							paving += 0.2f;
						else
							grass += 0.2f;
					}
					if (k != kWater && k != kBeach && d < 14)
					{
						const float t = Smooth(d / 14);
						grass *= t, paving *= t, soft = soft * t + (1 - t);
					}
					// (and a little darkening, noise over 29 and 71 m: the textures' repeats under larger patches - none finer than
					// a few vertices: 9 m noise at 8 m vertices was each vertex's own, every cell's shade a checkerboard square)
					auto layers = terrain::Layers(grass, paving, soft, wetSand);
					layers[0] = static_cast<uint8_t>(std::lround(std::clamp(0.1f + 0.08f * Lattice(x, y, 29, 71) + 0.05f * Lattice(x, y, 71, 73), 0.0f, 0.25f) * 255));
					f.layers.push_back(layers);
				}
			for (int j = 0; j < per; ++j)
				for (int i = 0; i < per; ++i)
				{
					const uint8_t what = coverAt(x0 + (i + 0.5f) * o.cell, y0 + (j + 0.5f) * o.cell);
					f.material.push_back(what == kUnder || what == kSandy ? kSand : what == kPaving ? kConcrete : what == kBare ? kDirt : kGrass);
				}
			// the forest floors: as the lawns (below), from 4 m in, easing over the next 10 m
			{
				const auto capable = [&](int vi, int vj) { return forestDepthAt(x0 + vi * o.cell, y0 + vj * o.cell) > 4; };
				const auto forestCell = [&](int ci, int cj) { return capable(ci, cj) && capable(ci + 1, cj) && capable(ci, cj + 1) && capable(ci + 1, cj + 1); };
				f.forestCell.resize(static_cast<size_t>(per) * per);
				for (int j = 0; j < per; ++j)
					for (int i = 0; i < per; ++i)
						f.forestCell[static_cast<size_t>(j) * per + i] = forestCell(i, j);
				f.forest.resize(static_cast<size_t>(per + 1) * (per + 1));
				for (int j = 0; j <= per; ++j)
					for (int i = 0; i <= per; ++i)
					{
						const bool inside = forestCell(i - 1, j - 1) && forestCell(i, j - 1) && forestCell(i - 1, j) && forestCell(i, j);
						f.forest[f.V(i, j)] = inside ? Smooth((forestDepthAt(x0 + i * o.cell, y0 + j * o.cell) - 4) / 10) : 0.0f;
					}
			}
			// the lawns: cells whose four corners are 6 m and more into a lawn; a vertex's depth into it eases over the
			// next 14 m, 0 where it touches a cell of the ground material (there both draw the same grass)
			{
				const auto capable = [&](int vi, int vj) { return depthAt(x0 + vi * o.cell, y0 + vj * o.cell) > 6; };
				const auto lawnCell = [&](int ci, int cj) { return capable(ci, cj) && capable(ci + 1, cj) && capable(ci, cj + 1) && capable(ci + 1, cj + 1); };
				f.lawnCell.resize(static_cast<size_t>(per) * per);
				for (int j = 0; j < per; ++j)
					for (int i = 0; i < per; ++i)
						f.lawnCell[static_cast<size_t>(j) * per + i] = lawnCell(i, j);
				f.lawn.resize(static_cast<size_t>(per + 1) * (per + 1));
				for (int j = 0; j <= per; ++j)
					for (int i = 0; i <= per; ++i)
					{
						const bool inside = lawnCell(i - 1, j - 1) && lawnCell(i, j - 1) && lawnCell(i - 1, j) && lawnCell(i, j);
						f.lawn[f.V(i, j)] = inside ? Smooth((depthAt(x0 + i * o.cell, y0 + j * o.cell) - 6) / 14) : 0.0f;
					}
			}
			if (!anyLand)
				return true; // (deep open sea: nothing to see or walk on)
			const std::string tname = std::format("{}_t_{}_{}", o.name, ti, tj), rname = std::format("{}_r_{}_{}", o.name, ti, tj),
			                  cname = std::format("{}_c_{}_{}", o.name, ti, tj);
			ydr::Drawable td;
			td.name = tname;
			// (a tile with sand by the sea or a beach: the sand; inland the forest floor)
			bool sandy = false;
			for (int j = 0; j <= per && !sandy; j += 2)
				for (int i = 0; i <= per && !sandy; i += 2)
				{
					const float x = x0 + i * o.cell, y = y0 + j * o.cell;
					const auto lc = land.Cell({x, y});
					sandy = land.In(lc[0], lc[1]) && (land.At(lc[0], lc[1]) == kBeach || (land.At(lc[0], lc[1]) == kWater && seaBody[static_cast<size_t>(lc[1]) * land.nx + lc[0]]) ||
					                                  coverAt(x, y) == kSandy);
				}
			td.materials = {sandy ? ground3 : groundForest, lawnMaterial, forestMaterial};
			{
				// (the textures every 3.5 m as the game's lawns have them, our blend map every 128 m)
				auto [rest, lawn, forest] = terrain::TileMeshes(f, 0, 0, per, cx, cy, 3.5f, 128.0f);
				lawn.material = 1;
				forest.material = 2;
				for (ydr::Mesh* m : {&rest, &lawn, &forest})
					if (!m->indices.empty())
					{
						r.terrainTriangles += m->indices.size() / 3;
						td.lods[0].push_back(std::move(*m));
					}
			}
			Bytes file;
			if (!ydr::Write(td, file, error))
				return error = tname + ": " + error, false;
			WriteFile(o.out / (tname + ".ydr"), file);
			// The water up the land in this tile (the game's sea renderer draws only the sea's level): a mesh of the river
			// water, 8 m cells, each corner at the surface there (a lake flat, a river following its levels).
			{
				const auto surfaceAt = [&](float x, float y) {
					const auto c = upLand.Cell({x, y});
					return upLand.In(c[0], c[1]) ? upLand.At(c[0], c[1]) : std::numeric_limits<float>::quiet_NaN();
				};
				constexpr float kWaterCell = 8;
				const int wn = static_cast<int>(std::lround(o.tile / kWaterCell));
				std::vector<float> corner(static_cast<size_t>(wn + 1) * (wn + 1), std::numeric_limits<float>::quiet_NaN());
				for (int j = 0; j <= wn; ++j)
					for (int i = 0; i <= wn; ++i)
					{
						float sum = 0;
						int count = 0;
						for (int dj = -1; dj <= 0; ++dj)
							for (int di = -1; di <= 0; ++di)
								if (const float lv = surfaceAt(x0 + (i + di + 0.5f) * kWaterCell, y0 + (j + dj + 0.5f) * kWaterCell); !std::isnan(lv))
									sum += lv, ++count;
						if (count)
							corner[static_cast<size_t>(j) * (wn + 1) + i] = sum / count;
					}
				ydr::Mesh wm;
				std::vector<int32_t> index(corner.size(), -1);
				const auto vertex = [&](int i, int j, float fallback) {
					const size_t k = static_cast<size_t>(j) * (wn + 1) + i;
					if (index[k] < 0)
					{
						const float x = x0 + i * kWaterCell, y = y0 + j * kWaterCell;
						ydr::Vertex v;
						v.position = {x - cx, y - cy, std::isnan(corner[k]) ? fallback : corner[k]};
						v.colour0 = {92, 34, 0, 255};
						v.uv[0] = {(x - cx) * 0.008f, -(y - cy) * 0.008f};
						index[k] = static_cast<int32_t>(wm.vertices.size());
						wm.vertices.push_back(v);
					}
					return static_cast<uint32_t>(index[k]);
				};
				for (int j = 0; j < wn; ++j)
					for (int i = 0; i < wn; ++i)
					{
						const float lv = surfaceAt(x0 + (i + 0.5f) * kWaterCell, y0 + (j + 0.5f) * kWaterCell);
						if (std::isnan(lv))
							continue;
						const uint32_t a = vertex(i, j, lv), b = vertex(i + 1, j, lv), c = vertex(i, j + 1, lv), d = vertex(i + 1, j + 1, lv);
						wm.indices.insert(wm.indices.end(), {a, b, d, a, d, c});
					}
				if (!wm.indices.empty())
				{
					const std::string wname = std::format("{}_w_{}_{}", o.name, ti, tj);
					ydr::Drawable wd;
					wd.name = wname;
					wd.materials = {riverMaterial};
					wd.lods[0].push_back(std::move(wm));
					if (!ydr::Write(wd, file, error))
						return error = wname + ": " + error, false;
					WriteFile(o.out / (wname + ".ydr"), file);
					mapfiles::Archetype wa;
					wa.name = wname;
					wa.textureDictionary = loader::convert::Joaat(facadeDictionary);
					wa.lodDist = 3000;
					wa.flags = 0x2000; // (water: drawn in the water's pass, as the game's ponds and rivers of water_river are)
					ydr::Box(wd, wa.min, wa.max);
					r.archetypes.push_back(wa);
					mapfiles::Entity we;
					we.archetype = loader::convert::Joaat(wname);
					we.position = {cx, cy, 0};
					we.lodDist = 3000;
					we.radius = o.tile * 0.75f;
					r.entities.push_back(we);
				}
			}
			mapfiles::Archetype ta;
			ta.name = tname;
			ta.textureDictionary = loader::convert::Joaat(facadeDictionary);
			ta.lodDist = 2500;
			ydr::Box(td, ta.min, ta.max);
			r.archetypes.push_back(ta);
			const int bi = static_cast<int>(std::floor((cx - w.min[0]) / 1000)), bj = static_cast<int>(std::floor((cy - w.min[1]) / 1000));
			r.block = {bi, bj};
			mapfiles::Entity te;
			te.archetype = loader::convert::Joaat(tname);
			te.position = {cx, cy, 0};
			te.lodDist = 2500;
			te.radius = o.tile * 0.75f;
			r.entities.push_back(te);
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
			for (const uint32_t t : collisionTiles.Near(ti, tj))
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
				r.roadTriangles += 1;
			}
			flush();
			// the buildings standing in this tile: entities, their boxes (obstacles for the navmesh)
			std::vector<P3> boxVertices;
			std::vector<std::array<uint32_t, 3>> boxTriangles;
			for (const uint32_t pk : placedTiles.Near(ti, tj))
			{
				const auto& p = placed[pk];
				if (p.centre[0] < x0 || p.centre[0] >= x1 || p.centre[1] < y0 || p.centre[1] >= y1)
					continue;
				mapfiles::Entity be;
				be.archetype = p.item->hash;
				be.position = p.position;
				be.rotation = {0, 0, std::sin(p.heading / 2), std::cos(p.heading / 2)};
				be.lodDist = p.lodDist;
				be.radius = std::hypot(p.item->max[0] - p.item->min[0], p.item->max[1] - p.item->min[1], p.item->max[2] - p.item->min[2]) / 2;
				r.entities.push_back(be);
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
				const std::vector<uint32_t> nearShapes = shapeTiles.Near(ti, tj);
				std::map<uint32_t, roads::Mesh> meshes;
				std::vector<P3> cv;
				std::vector<std::array<uint32_t, 3>> ct;
				const uint32_t aerialKey = static_cast<uint32_t>(facadeSet.styles.size()), gravelKey = static_cast<uint32_t>(gravelStyle);
				float tallest = 0;
				size_t standing = 0;
				for (const uint32_t k : nearShapes)
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
							r.parents.push_back({txd, facadeDictionary});
						}
						else
						{
							// (no picture after all: the gravel)
							auto& from = meshes[aerialKey];
							auto& to = meshes[gravelKey];
							const uint32_t base = static_cast<uint32_t>(to.vertices.size());
							for (auto v : from.vertices)
							{
								v.uv[0] = {v.position[0] / facadeSet.styles[gravelStyle].width, -v.position[1] / facadeSet.styles[gravelStyle].height};
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
					r.archetypes.push_back(ba);
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
						for (const uint32_t k : nearShapes)
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
								r.parents.push_back({ltxd, facadeDictionary});
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
							r.archetypes.push_back(la);
							mapfiles::Entity le;
							le.archetype = loader::convert::Joaat(lname);
							le.position = {cx, cy, 0};
							le.lodDist = la.lodDist;
							le.lodLevel = 1;
							le.numChildren = 1;
							le.childLodDist = be.lodDist;
							le.radius = be.radius;
							be.lodLevel = 0;
							r.hasLod = true, r.lod = le; // (its place in the block's LOD map: when the tiles are put together)
						}
					}
					if (r.hasLod)
						r.lodOwner = r.entities.size();
					r.entities.push_back(be);
					r.generatedTiles += 1;
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
			for (const uint32_t fk : furnitureTiles.Near(ti, tj))
			{
				const auto& prop = furniture[fk];
				if (prop.position[0] < x0 || prop.position[0] >= x1 || prop.position[1] < y0 || prop.position[1] >= y1)
					continue;
				mapfiles::Entity pe;
				pe.archetype = prop.archetype;
				pe.position = prop.position;
				pe.rotation = {0, 0, std::sin(prop.heading / 2), std::cos(prop.heading / 2)};
				pe.lodDist = prop.lodDist;
				pe.scaleXY = pe.scaleZ = prop.scale;
				pe.radius = 6 * prop.scale;
				r.entities.push_back(pe);
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
				const uint32_t base = static_cast<uint32_t>(r.nav.vertices.size());
				r.nav.vertices.insert(r.nav.vertices.end(), boxVertices.begin(), boxVertices.end());
				for (const auto& t : boxTriangles)
				{
					r.nav.triangles.push_back({base + t[0], base + t[1], base + t[2]});
					r.nav.materials.push_back(kConcrete);
					r.navSurfaces.push_back(navgen::Surface::None);
				}
			}
			std::set<std::array<P3, 3>> boxSet;
			for (const auto& t : boxTriangles)
				boxSet.insert({boxVertices[t[0]], boxVertices[t[1]], boxVertices[t[2]]});
			for (size_t pi = 0; pi < parts.size(); ++pi)
			{
				const ybn::Part& p = parts[pi];
				const auto kinds = roadKinds.find(pi);
				const uint32_t base = static_cast<uint32_t>(r.nav.vertices.size());
				r.nav.vertices.insert(r.nav.vertices.end(), p.vertices.begin(), p.vertices.end());
				for (size_t t = 0; t < p.triangles.size(); ++t)
				{
					if (boxSet.contains({p.vertices[p.triangles[t][0]], p.vertices[p.triangles[t][1]], p.vertices[p.triangles[t][2]]}))
						continue; // (a building's: added above)
					r.nav.triangles.push_back({base + p.triangles[t][0], base + p.triangles[t][1], base + p.triangles[t][2]});
					const uint8_t m = p.materials[p.triangleMaterials[t]][0];
					r.nav.materials.push_back(m);
					const uint8_t kind = kinds != roadKinds.end() ? kinds->second[t] : 0;
					r.navSurfaces.push_back(kind == 1 ? navgen::Surface::Road
					                      : kind == 2 || (kind == 0 && (m == 1 || m == 15 || m == 174)) ? navgen::Surface::Pavement
					                                                                                    : navgen::Surface::Ground);
				}
			}
			// the roads' model
			// (its materials those it uses here; the wear's decals before the markings, which are painted over them)
			ydr::Drawable rd;
			rd.name = rname;
			const auto add = [&](const ydr::Material& mat, const roads::Mesh& mesh) {
				const auto near = meshTiles.at(&mesh).Near(ti, tj);
				const roads::Mesh part = Clip(mesh, x0, y0, x1, y1, cx, cy, &near);
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
				r.archetypes.push_back(ra);
				mapfiles::Entity re;
				re.archetype = loader::convert::Joaat(rname);
				re.position = {cx, cy, 0};
				re.lodDist = 1200;
				re.radius = o.tile * 0.75f;
				r.entities.push_back(re);
			}
			++r.tiles;
			return true;
		};
		{
			const int across = ti1 - ti0, count = across * (tj1 - tj0);
			std::vector<TileResult> results(static_cast<size_t>(std::max(0, count)));
			std::atomic<int> next = 0;
			std::atomic<bool> failed = false;
			std::string firstError;
			std::mutex errorMutex;
			std::vector<std::thread> workers;
			for (unsigned k = 0; k < std::max(1u, std::thread::hardware_concurrency()); ++k)
				workers.emplace_back([&] {
					elevation::Aerial photo;
					if (pictured)
						photo.Open(o.aerial);
					std::string why;
					for (int n; !failed && (n = next++) < count;)
						if (!makeTile(ti0 + n % across, tj0 + n / across, photo, results[static_cast<size_t>(n)], why))
						{
							std::lock_guard lock(errorMutex);
							if (!failed.exchange(true))
								firstError = why;
						}
				});
			for (auto& worker : workers)
				worker.join();
			if (failed)
				return error = firstError, false;
			for (TileResult& r : results)
			{
				archetypes.insert(archetypes.end(), r.archetypes.begin(), r.archetypes.end());
				if (r.hasLod)
				{
					auto& lods = lodBlocks[r.block];
					r.entities[r.lodOwner].parentIndex = static_cast<int32_t>(lods.size());
					lods.push_back(r.lod);
				}
				auto& block = blocks[r.block];
				block.insert(block.end(), r.entities.begin(), r.entities.end());
				parents.insert(parents.end(), r.parents.begin(), r.parents.end());
				const uint32_t base = static_cast<uint32_t>(navMesh.vertices.size());
				navMesh.vertices.insert(navMesh.vertices.end(), r.nav.vertices.begin(), r.nav.vertices.end());
				for (const auto& tri : r.nav.triangles)
					navMesh.triangles.push_back({base + tri[0], base + tri[1], base + tri[2]});
				navMesh.materials.insert(navMesh.materials.end(), r.nav.materials.begin(), r.nav.materials.end());
				navSurfaces.insert(navSurfaces.end(), r.navSurfaces.begin(), r.navSurfaces.end());
				stats.terrainTriangles += r.terrainTriangles, stats.roadTriangles += r.roadTriangles;
				stats.generatedTiles += r.generatedTiles, stats.tiles += r.tiles;
			}
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
		took("tiles");
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
		took("archetypes and maps");
		// 6. pedestrians' navmeshes (the sea floor half a metre down and more left out)
		if (o.navmesh && !navMesh.triangles.empty())
		{
			for (size_t t = 0; t < navMesh.triangles.size(); ++t)
				if (std::all_of(navMesh.triangles[t].begin(), navMesh.triangles[t].end(), [&](uint32_t v) {
					    const float lv = waterLevelAt(navMesh.vertices[v][0], navMesh.vertices[v][1]);
					    return navMesh.vertices[v][2] < (std::isnan(lv) ? 0.0f : lv) - 0.5f;
				    }))
					navSurfaces[t] = navgen::Surface::None;
			if (o.navmeshNearOnly)
			{
				// (the navmesh grid's squares with something people walk to in them - pavements, buildings, beaches, paved
				// ground; a country road's own carriageway not: no one walks along it - the ground and the roads elsewhere not
				// walkable, each triangle by where its middle is)
				const auto squareOf = [](float x, float y) {
					return std::make_pair(static_cast<int>(std::floor((x + 6000) / 150)), static_cast<int>(std::floor((y + 6000) / 150)));
				};
				const auto middle = [&](size_t t) {
					const auto& tri = navMesh.triangles[t];
					return squareOf((navMesh.vertices[tri[0]][0] + navMesh.vertices[tri[1]][0] + navMesh.vertices[tri[2]][0]) / 3,
					    (navMesh.vertices[tri[0]][1] + navMesh.vertices[tri[1]][1] + navMesh.vertices[tri[2]][1]) / 3);
				};
				std::set<std::pair<int, int>> wanted;
				for (size_t t = 0; t < navMesh.triangles.size(); ++t)
					if (navSurfaces[t] == navgen::Surface::Pavement)
						wanted.insert(middle(t));
				for (const auto& b : w.buildings)
					if (!b.footprint.empty())
						wanted.insert(squareOf(b.footprint.front()[0], b.footprint.front()[1]));
				for (int j = 0; j < land.ny; ++j)
					for (int i = 0; i < land.nx; ++i)
						if (const uint8_t k = land.At(i, j); k == kBeach || k == kPaved) // (not parks: woods and wetlands are those too)
						{
							const P2 c = land.Centre(i, j);
							wanted.insert(squareOf(c[0], c[1]));
						}
				size_t dropped = 0;
				for (size_t t = 0; t < navMesh.triangles.size(); ++t)
					if ((navSurfaces[t] == navgen::Surface::Ground || navSurfaces[t] == navgen::Surface::Road) && !wanted.contains(middle(t)))
						navSurfaces[t] = navgen::Surface::None, ++dropped;
				std::printf("navmeshes: %zu squares with pavements, buildings, beaches or paving; %zu ground and road triangles elsewhere left out\n", wanted.size(), dropped);
			}
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
		took("navmeshes");
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
		took("traffic");
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
		took("zones");
		return true;
	}
}
