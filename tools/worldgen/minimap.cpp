#include "minimap.hpp"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <format>
#include <fstream>
#include <limits>
#include <map>
#include <numbers>
#include <unordered_map>

#include "../ue5export/drawable.hpp"
#include "convert/ytd.hpp"
#include "polygon.hpp"
#include "texture.hpp"
#include "ydr.hpp"

namespace worldgen::minimap
{
	namespace
	{
		using loader::convert::Bytes;

		// The radar's lattice (minimap.ymt) and our supertiles' numbers on it.
		constexpr float kRadarX = -4500, kRadarY = 8000, kSuperW = 9400.0f / 8, kSuperH = 12492.0f / 9;
		constexpr int kIndexBase = 1000;
		// The pause map's picture lattice (minimap.ymt), the game's own squares on it, the pictures' sides.
		constexpr float kPictureX = -4140, kPictureY = 8400, kPicture = 4500;
		constexpr int kGameRows = 3, kGameColumns = 2;
		constexpr int kLandSide = 512, kSeaSide = 1024;

		using Colour = std::array<uint8_t, 4>;
		// The game's colours (its radar tiles and pictures): the land, buildings, the open sea (past the deepest band the
		// radar draws nothing: its own background; the land's picture this, the sea's nothing), and the water's depth bands
		// from the shore out, each with its alpha in the sea's picture.
		constexpr Colour kLand{29, 29, 29, 255}, kBuilding{65, 65, 65, 255}, kOpenSea{57, 73, 79, 255}, kNoSea{255, 255, 255, 0};
		struct Band
		{
			float depth; // down to (m)
			Colour colour;
			uint8_t alpha;
		};
		constexpr Band kBands[] = {{1, {215, 221, 224, 255}, 153}, {2, {194, 206, 211, 255}, 148}, {3, {186, 198, 204, 255}, 137},
		    {4, {162, 179, 186, 255}, 125}, {5, {151, 167, 175, 255}, 115}, {6.5f, {140, 157, 165, 255}, 105}, {8, {126, 145, 153, 255}, 95},
		    {10, {113, 133, 142, 255}, 80}, {12, {100, 120, 130, 255}, 65}, {14, {91, 110, 118, 255}, 53}, {16, {87, 107, 116, 255}, 40},
		    {19, {77, 97, 106, 255}, 30}, {22, {68, 87, 95, 255}, 20}, {25, {61, 79, 85, 255}, 10}};
		constexpr int kBandCount = static_cast<int>(std::size(kBands));
		// The heights (z) the game's tiles draw at: the water, the land, buildings, roads (lesser roads a little lower,
		// under the streets they meet).
		constexpr float kZSea = -12, kZLand = 0, kZBuilding = 6, kZRoad = 12;
		constexpr float kOutside = -1000; // the ground out of the box: the open sea's bottom

		struct Tri
		{
			std::array<P2, 3> p;
			Colour colour;
			float z;
			uint8_t alpha = 255; // in the sea's picture
		};

		// A supertile's triangles: the land, the water, the foreground's nine tiles (by x, then y from the north-west).
		struct Supertile
		{
			int column = 0, row = 0;
			std::vector<Tri> land, sea;
			std::array<std::vector<Tri>, 9> fore;
			float X0() const { return kRadarX + column * kSuperW; }
			float Y1() const { return kRadarY - row * kSuperH; }
		};

		struct Rect
		{
			float x0, y0, x1, y1;
		};

		// A point with the ground's height there (interpolated along the edges).
		struct V
		{
			P2 p;
			float f;
		};

		// The part of the polygon where f >= level (keepAbove) or f < level.
		std::vector<V> ClipLevel(const std::vector<V>& poly, float level, bool keepAbove)
		{
			std::vector<V> out;
			for (size_t k = 0; k < poly.size(); ++k)
			{
				const V& a = poly[k];
				const V& b = poly[(k + 1) % poly.size()];
				const bool ina = keepAbove ? a.f >= level : a.f < level, inb = keepAbove ? b.f >= level : b.f < level;
				if (ina)
					out.push_back(a);
				if (ina != inb)
				{
					const float t = (level - a.f) / (b.f - a.f);
					out.push_back({{a.p[0] + t * (b.p[0] - a.p[0]), a.p[1] + t * (b.p[1] - a.p[1])}, level});
				}
			}
			return out;
		}

		// The part inside the rectangle.
		std::vector<V> ClipRect(std::vector<V> poly, const Rect& r)
		{
			const auto side = [&](int axis, float bound, bool keepAbove) {
				std::vector<V> out;
				for (size_t k = 0; k < poly.size(); ++k)
				{
					const V& a = poly[k];
					const V& b = poly[(k + 1) % poly.size()];
					const bool ina = keepAbove ? a.p[axis] >= bound : a.p[axis] <= bound, inb = keepAbove ? b.p[axis] >= bound : b.p[axis] <= bound;
					if (ina)
						out.push_back(a);
					if (ina != inb)
					{
						const float t = (bound - a.p[axis]) / (b.p[axis] - a.p[axis]);
						V c{{a.p[0] + t * (b.p[0] - a.p[0]), a.p[1] + t * (b.p[1] - a.p[1])}, a.f + t * (b.f - a.f)};
						c.p[axis] = bound;
						out.push_back(c);
					}
				}
				poly = std::move(out);
			};
			side(0, r.x0, true);
			if (poly.size() >= 3)
				side(0, r.x1, false);
			if (poly.size() >= 3)
				side(1, r.y0, true);
			if (poly.size() >= 3)
				side(1, r.y1, false);
			return poly;
		}

		void Fan(const std::vector<V>& poly, const Colour& colour, float z, uint8_t alpha, std::vector<Tri>& out)
		{
			for (size_t k = 1; k + 1 < poly.size(); ++k)
				out.push_back({{poly[0].p, poly[k].p, poly[k + 1].p}, colour, z, alpha});
		}

		// The land (the ground at the sea level or above) or the water's bands over the rectangle, from the ground's
		// heights at the centres of every `step`-th cell (squares between four of them; out of the grid or the box: the
		// open sea): squares wholly of one kind merged into rectangles, the rest cut along the heights' level lines.
		void Regions(const raster::Grid<float>& g, int step, const Rect& box, const Rect& r, bool land, std::vector<Tri>& out)
		{
			const float cell = g.cell * step;
			const float ox = g.x0 + 0.5f * g.cell, oy = g.y0 + 0.5f * g.cell;
			const int i0 = static_cast<int>(std::floor((r.x0 - ox) / cell)), i1 = static_cast<int>(std::floor((r.x1 - ox) / cell));
			const int j0 = static_cast<int>(std::floor((r.y0 - oy) / cell)), j1 = static_cast<int>(std::floor((r.y1 - oy) / cell));
			const int nx = i1 - i0 + 1, ny = j1 - j0 + 1;
			if (nx <= 0 || ny <= 0)
				return;
			const auto value = [&](int i, int j) {
				const int gi = i * step, gj = j * step;
				const float x = ox + gi * g.cell, y = oy + gj * g.cell;
				if (gi < 0 || gj < 0 || gi >= g.nx || gj >= g.ny || x < box.x0 || x > box.x1 || y < box.y0 || y > box.y1)
					return kOutside;
				return g.At(gi, gj);
			};
			struct Layer
			{
				float lo, hi; // [lo, hi)
				Colour colour;
				float z;
				uint8_t alpha;
			};
			std::vector<Layer> layers;
			if (land)
				layers.push_back({0, std::numeric_limits<float>::infinity(), kLand, kZLand, 255});
			else
				for (int k = 0; k < kBandCount; ++k)
					layers.push_back({-kBands[k].depth, k ? -kBands[k - 1].depth : 0.0f, kBands[k].colour, kZSea, kBands[k].alpha});
			std::vector<std::vector<uint8_t>> full(layers.size(), std::vector<uint8_t>(static_cast<size_t>(nx) * ny, 0));
			for (int j = 0; j < ny; ++j)
				for (int i = 0; i < nx; ++i)
				{
					const int ci = i0 + i, cj = j0 + j;
					const float x0 = ox + ci * cell, y0 = oy + cj * cell, x1 = x0 + cell, y1 = y0 + cell;
					if (x1 <= r.x0 || x0 >= r.x1 || y1 <= r.y0 || y0 >= r.y1)
						continue;
					const float v[4] = {value(ci, cj), value(ci + 1, cj), value(ci + 1, cj + 1), value(ci, cj + 1)};
					const float lo = std::min({v[0], v[1], v[2], v[3]}), hi = std::max({v[0], v[1], v[2], v[3]});
					const bool inside = x0 >= r.x0 && x1 <= r.x1 && y0 >= r.y0 && y1 <= r.y1;
					for (size_t l = 0; l < layers.size(); ++l)
					{
						const Layer& layer = layers[l];
						if (hi < layer.lo || lo >= layer.hi)
							continue;
						if (inside && lo >= layer.lo && hi < layer.hi)
						{
							full[l][static_cast<size_t>(j) * nx + i] = 1;
							continue;
						}
						std::vector<V> poly{{{x0, y0}, v[0]}, {{x1, y0}, v[1]}, {{x1, y1}, v[2]}, {{x0, y1}, v[3]}};
						poly = ClipLevel(poly, layer.lo, true);
						if (poly.size() >= 3 && std::isfinite(layer.hi))
							poly = ClipLevel(poly, layer.hi, false);
						if (poly.size() >= 3 && !inside)
							poly = ClipRect(poly, r);
						if (poly.size() >= 3)
							Fan(poly, layer.colour, layer.z, layer.alpha, out);
					}
				}
			// (the whole squares: the widest run along x, then as many rows of it as there are)
			for (size_t l = 0; l < layers.size(); ++l)
			{
				auto& f = full[l];
				const Layer& layer = layers[l];
				for (int j = 0; j < ny; ++j)
					for (int i = 0; i < nx; ++i)
					{
						if (!f[static_cast<size_t>(j) * nx + i])
							continue;
						int w = 1, h = 1;
						while (i + w < nx && f[static_cast<size_t>(j) * nx + i + w])
							++w;
						for (; j + h < ny; ++h)
						{
							bool all = true;
							for (int k = 0; k < w && all; ++k)
								all = f[static_cast<size_t>(j + h) * nx + i + k] != 0;
							if (!all)
								break;
						}
						for (int b = 0; b < h; ++b)
							std::fill_n(f.begin() + static_cast<size_t>(j + b) * nx + i, w, uint8_t{0});
						const float x0 = ox + (i0 + i) * cell, y0 = oy + (j0 + j) * cell, x1 = x0 + w * cell, y1 = y0 + h * cell;
						out.push_back({{P2{x0, y0}, P2{x1, y0}, P2{x1, y1}}, layer.colour, layer.z, layer.alpha});
						out.push_back({{P2{x0, y0}, P2{x1, y1}, P2{x0, y1}}, layer.colour, layer.z, layer.alpha});
					}
			}
		}

		// A road's shape: quads along it (pieces of 40 m at most, so that each lands in the tile it crosses) and discs at
		// its ends and where it bends more than 15 degrees (round joints: at a gentler bend the quads' gap is a fraction of
		// a pixel on the radar), anticlockwise.
		std::vector<std::vector<V>> RoadShape(const Road& road)
		{
			std::vector<std::vector<V>> out;
			const float w = road.halfWidth;
			for (size_t k = 0; k + 1 < road.points.size(); ++k)
			{
				const P2 a = road.points[k], b = road.points[k + 1];
				const float dx = b[0] - a[0], dy = b[1] - a[1], length = std::hypot(dx, dy);
				if (length < 0.01f)
					continue;
				const int pieces = std::max(1, static_cast<int>(std::ceil(length / 40)));
				const float nx = -dy / length * w, ny = dx / length * w; // (to the left)
				for (int p = 0; p < pieces; ++p)
				{
					const float t0 = static_cast<float>(p) / pieces, t1 = static_cast<float>(p + 1) / pieces;
					const P2 s{a[0] + dx * t0, a[1] + dy * t0}, e{a[0] + dx * t1, a[1] + dy * t1};
					out.push_back({{{s[0] - nx, s[1] - ny}, 0}, {{e[0] - nx, e[1] - ny}, 0}, {{e[0] + nx, e[1] + ny}, 0}, {{s[0] + nx, s[1] + ny}, 0}});
				}
			}
			const float bend = std::cos(15 * std::numbers::pi_v<float> / 180);
			for (size_t k = 0; k < road.points.size(); ++k)
			{
				const P2& p = road.points[k];
				if (k > 0 && k + 1 < road.points.size())
				{
					const P2 &a = road.points[k - 1], &b = road.points[k + 1];
					const float ax = p[0] - a[0], ay = p[1] - a[1], bx = b[0] - p[0], by = b[1] - p[1];
					const float la = std::hypot(ax, ay), lb = std::hypot(bx, by);
					if (la > 0.01f && lb > 0.01f && (ax * bx + ay * by) / (la * lb) > bend)
						continue;
				}
				std::vector<V> disc;
				for (int s = 0; s < 8; ++s)
				{
					const float a = (s + 0.5f) * 2 * std::numbers::pi_v<float> / 8;
					disc.push_back({{p[0] + w * std::cos(a), p[1] + w * std::sin(a)}, 0});
				}
				out.push_back(std::move(disc));
			}
			return out;
		}

		// A drawable of the triangles (the lowest drawn first), its vertices shared where position and colour agree.
		ydr::Drawable MakeDrawable(const std::string& name, const ydr::Material& material, std::vector<Tri> tris)
		{
			std::stable_sort(tris.begin(), tris.end(), [](const Tri& a, const Tri& b) { return a.z < b.z; });
			ydr::Drawable d;
			d.name = name;
			d.materials = {material};
			struct Key
			{
				float x, y, z;
				uint32_t colour;
				bool operator==(const Key&) const = default;
			};
			struct Hash
			{
				size_t operator()(const Key& k) const
				{
					uint32_t a, b, c;
					std::memcpy(&a, &k.x, 4), std::memcpy(&b, &k.y, 4), std::memcpy(&c, &k.z, 4);
					return (static_cast<size_t>(a) * 73856093u) ^ (static_cast<size_t>(b) * 19349663u) ^ (static_cast<size_t>(c) * 83492791u) ^ k.colour;
				}
			};
			ydr::Mesh mesh;
			std::unordered_map<Key, uint32_t, Hash> index;
			for (const Tri& t : tris)
			{
				if (mesh.vertices.size() + 3 > 0xFFFF)
				{
					d.lods[0].push_back(std::move(mesh));
					mesh = {};
					index.clear();
				}
				for (const P2& p : t.p)
				{
					uint32_t colour;
					std::memcpy(&colour, t.colour.data(), 4);
					const Key key{p[0], p[1], t.z, colour};
					auto it = index.find(key);
					if (it == index.end())
					{
						ydr::Vertex v;
						v.position = {p[0], p[1], t.z};
						v.colour0 = t.colour;
						mesh.vertices.push_back(v);
						it = index.emplace(key, static_cast<uint32_t>(mesh.vertices.size() - 1)).first;
					}
					mesh.indices.push_back(it->second);
				}
			}
			if (!mesh.indices.empty())
				d.lods[0].push_back(std::move(mesh));
			return d;
		}

		// The shader of the game's radar tiles (a model of one of them).
		bool RadarMaterial(const ue5export::GameFiles& files, ydr::Material& out, std::string& error)
		{
			Bytes data;
			loader::convert::Resource res;
			const auto* f = files.Find("minimap_4_4.ydd");
			if (!f || !files.Read(*f, data, error) || !loader::convert::Read(data, res, error))
				return error = "the game's radar tile minimap_4_4.ydd: " + error, false;
			const ue5export::drawable::View view(res);
			const auto drawables = ue5export::drawable::Drawables(view, "ydd");
			if (drawables.empty())
				return error = "minimap_4_4.ydd has no models", false;
			return ydr::ReadMaterial(view, drawables.begin()->second, 0, out, error);
		}

		// The game's own picture (the first texture of the dictionary of that name, decoded).
		bool GamePicture(const ue5export::GameFiles& files, const std::string& name, texture::Image& out, std::string& error)
		{
			Bytes data;
			loader::convert::Resource res;
			const auto* f = files.Find(name + ".ytd");
			if (!f || !files.Read(*f, data, error) || !loader::convert::Read(data, res, error))
				return error = "cannot read " + name + ".ytd: " + error, false;
			const auto textures = ue5export::drawable::Textures(ue5export::drawable::View(res), ue5export::drawable::View::kRoot);
			if (textures.empty())
				return error = name + ".ytd has no texture", false;
			const auto& t = textures[0];
			loader::convert::TextureFormat format{t.dxgi, t.dxgi == 71 ? 8u : 16u, true};
			uint8_t mips = t.mips;
			Bytes pixels(t.data.begin(), t.data.end());
			loader::convert::MakeRenderTargetFormat(format, mips, static_cast<uint16_t>(t.width), static_cast<uint16_t>(t.height), pixels);
			out = texture::Image(t.width, t.height);
			for (size_t i = 0; i < out.pixels.size() && 4 * i + 3 < pixels.size(); ++i)
				out.pixels[i] = {pixels[4 * i + 2], pixels[4 * i + 1], pixels[4 * i], pixels[4 * i + 3]};
			return true;
		}

		// Triangles drawn into a picture whose north-west corner is (x0, y1), `metres` a pixel: each pixel whose centre is
		// in one takes its colour (in the sea's picture the water's alpha).
		void Draw(texture::Image& img, float x0, float y1, float metres, const std::vector<const Tri*>& tris, bool sea)
		{
			for (const Tri* t : tris)
			{
				float u[3], v[3];
				for (int k = 0; k < 3; ++k)
					u[k] = (t->p[k][0] - x0) / metres, v[k] = (y1 - t->p[k][1]) / metres;
				const int px0 = std::max(0, static_cast<int>(std::floor(std::min({u[0], u[1], u[2]})))),
				          px1 = std::min(img.width - 1, static_cast<int>(std::ceil(std::max({u[0], u[1], u[2]})))),
				          py0 = std::max(0, static_cast<int>(std::floor(std::min({v[0], v[1], v[2]})))),
				          py1 = std::min(img.height - 1, static_cast<int>(std::ceil(std::max({v[0], v[1], v[2]}))));
				if (px0 > px1 || py0 > py1)
					continue;
				const float area = (u[1] - u[0]) * (v[2] - v[0]) - (u[2] - u[0]) * (v[1] - v[0]);
				if (std::fabs(area) < 1e-9f)
					continue;
				const float s = area > 0 ? 1.0f : -1.0f;
				const Colour c = sea && t->z == kZSea ? Colour{t->colour[0], t->colour[1], t->colour[2], t->alpha} : t->colour;
				for (int py = py0; py <= py1; ++py)
					for (int px = px0; px <= px1; ++px)
					{
						const float x = px + 0.5f, y = py + 0.5f;
						const float e0 = s * ((u[1] - u[0]) * (y - v[0]) - (v[1] - v[0]) * (x - u[0]));
						const float e1 = s * ((u[2] - u[1]) * (y - v[1]) - (v[2] - v[1]) * (x - u[1]));
						const float e2 = s * ((u[0] - u[2]) * (y - v[2]) - (v[0] - v[2]) * (x - u[2]));
						if (e0 >= 0 && e1 >= 0 && e2 >= 0)
							img.At(px, py) = c;
					}
			}
		}

		// Half the size: each pixel the mean of four.
		texture::Image Halve(const texture::Image& big)
		{
			texture::Image small(big.width / 2, big.height / 2);
			for (int y = 0; y < small.height; ++y)
				for (int x = 0; x < small.width; ++x)
				{
					std::array<int, 4> sum{};
					for (int k = 0; k < 4; ++k)
					{
						const auto& p = big.At(2 * x + (k & 1), 2 * y + (k >> 1));
						for (int c = 0; c < 4; ++c)
							sum[c] += p[c];
					}
					for (int c = 0; c < 4; ++c)
						small.At(x, y)[c] = static_cast<uint8_t>((sum[c] + 2) / 4);
				}
			return small;
		}

		bool WritePicture(const std::filesystem::path& out, const std::string& name, const texture::Image& image, texture::Format format, std::string& error)
		{
			Bytes file;
			if (!loader::convert::WriteTextureDictionary({texture::Make(name, image, format)}, file, error))
				return false;
			std::ofstream o(out / (name + ".ytd"), std::ios::binary);
			o.write(reinterpret_cast<const char*>(file.data()), static_cast<std::streamsize>(file.size()));
			return static_cast<bool>(o) || (error = "cannot write " + name + ".ytd", false);
		}
	}

	bool Write(const ue5export::GameFiles& files, const Input& in, const std::string& name, const std::filesystem::path& out, Output& result,
	    std::string& error)
	{
		if (!in.height)
			return error = "no ground heights", false;
		ydr::Material material;
		if (!RadarMaterial(files, material, error))
			return false;
		const Rect box{in.box[0], in.box[1], in.box[2], in.box[3]};

		// the radar's supertiles over the box: the land, the water
		std::map<std::pair<int, int>, Supertile> tiles;
		const int c0 = static_cast<int>(std::floor((box.x0 - kRadarX) / kSuperW)), c1 = static_cast<int>(std::floor((box.x1 - 0.01f - kRadarX) / kSuperW));
		const int r0 = static_cast<int>(std::floor((kRadarY - box.y1) / kSuperH)), r1 = static_cast<int>(std::floor((kRadarY - box.y0 - 0.01f) / kSuperH));
		for (int row = r0; row <= r1; ++row)
			for (int column = c0; column <= c1; ++column)
			{
				Supertile s;
				s.column = column, s.row = row;
				const Rect r{std::max(s.X0(), box.x0), std::max(s.Y1() - kSuperH, box.y0), std::min(s.X0() + kSuperW, box.x1), std::min(s.Y1(), box.y1)};
				if (r.x1 <= r.x0 || r.y1 <= r.y0)
					continue;
				Regions(*in.height, 1, box, r, true, s.land);
				Regions(*in.height, 2, box, r, false, s.sea);
				tiles.emplace(std::make_pair(column, row), std::move(s));
			}
		// the foreground: each triangle into the tile its middle is in
		const auto fore = [&](const Tri& t) {
			const float cx = (t.p[0][0] + t.p[1][0] + t.p[2][0]) / 3, cy = (t.p[0][1] + t.p[1][1] + t.p[2][1]) / 3;
			const auto it = tiles.find({static_cast<int>(std::floor((cx - kRadarX) / kSuperW)), static_cast<int>(std::floor((kRadarY - cy) / kSuperH))});
			if (it == tiles.end())
				return;
			const int i = std::clamp(static_cast<int>(std::floor((cx - it->second.X0()) / (kSuperW / 3))), 0, 2);
			const int k = std::clamp(static_cast<int>(std::floor((it->second.Y1() - cy) / (kSuperH / 3))), 0, 2);
			it->second.fore[k * 3 + i].push_back(t);
		};
		for (const auto& footprint : in.buildings)
		{
			if (footprint.size() < 3)
				continue;
			P2 c{0, 0};
			for (const P2& q : footprint)
				c[0] += q[0] / footprint.size(), c[1] += q[1] / footprint.size();
			if (c[0] < box.x0 || c[0] >= box.x1 || c[1] < box.y0 || c[1] >= box.y1)
				continue;
			const auto ring = polygon::Clean(footprint, 0.05f);
			if (ring.size() < 3)
				continue;
			for (const auto& t : polygon::Triangulate(ring))
				fore({{ring[t[0]], ring[t[1]], ring[t[2]]}, kBuilding, kZBuilding});
		}
		for (const Road& road : in.roads)
		{
			const Colour colour{road.grey, road.grey, road.grey, 255};
			const float z = kZRoad - (180 - std::min<int>(road.grey, 180)) / 500.0f;
			for (auto& shape : RoadShape(road))
			{
				const auto poly = ClipRect(std::move(shape), box);
				std::vector<Tri> tris;
				if (poly.size() >= 3)
					Fan(poly, colour, z, 255, tris);
				for (const Tri& t : tris)
					fore(t);
			}
		}

		// the radar's dictionaries
		for (auto& [key, s] : tiles)
		{
			const int c = s.column + kIndexBase, r = s.row + kIndexBase;
			std::vector<std::pair<uint32_t, ydr::Drawable>> drawables;
			const auto add = [&](const std::string& drawable, const std::vector<Tri>& tris) {
				if (tris.empty())
					return;
				result.triangles += tris.size();
				drawables.emplace_back(loader::convert::Joaat(drawable), MakeDrawable(drawable, material, tris));
			};
			add(std::format("supertile_back_{}_{}", c, r), s.land);
			add(std::format("supertile_sea_{}_{}", c, r), s.sea);
			for (int k = 0; k < 3; ++k)
				for (int i = 0; i < 3; ++i)
					add(std::format("supertile_fore_{}_{}_tile_{}_{}", c, r, i, k), s.fore[k * 3 + i]);
			if (drawables.empty())
				continue;
			Bytes file;
			if (!ydr::WriteDictionary(drawables, file, error))
				return error = std::format("radar supertile {} {}: {}", s.column, s.row, error), false;
			std::ofstream o(out / std::format("minimap_{}_{}.ydd", c, r), std::ios::binary);
			o.write(reinterpret_cast<const char*>(file.data()), static_cast<std::streamsize>(file.size()));
			if (!o)
				return error = "cannot write a radar supertile", false;
			result.supertiles.push_back({s.column, s.row});
		}

		// the pause map's pictures: drawn at twice the size, then halved
		std::vector<const Tri*> all;
		for (const auto& [key, s] : tiles)
		{
			for (const auto* list : {&s.sea, &s.land})
				for (const Tri& t : *list)
					all.push_back(&t);
			for (const auto& tile : s.fore)
				for (const Tri& t : tile)
					all.push_back(&t);
		}
		std::stable_sort(all.begin(), all.end(), [](const Tri* a, const Tri* b) { return a->z < b->z; });
		const int pr0 = static_cast<int>(std::floor((kPictureY - box.y1) / kPicture)), pr1 = static_cast<int>(std::floor((kPictureY - box.y0 - 0.01f) / kPicture));
		const int pc0 = static_cast<int>(std::floor((box.x0 - kPictureX) / kPicture)), pc1 = static_cast<int>(std::floor((box.x1 - 0.01f - kPictureX) / kPicture));
		for (int row = pr0; row <= pr1; ++row)
			for (int column = pc0; column <= pc1; ++column)
			{
				const float x0 = kPictureX + column * kPicture, y1 = kPictureY - row * kPicture;
				const bool game = row >= 0 && row < kGameRows && column >= 0 && column < kGameColumns;
				std::vector<const Tri*> here;
				for (const Tri* t : all)
				{
					const float tx0 = std::min({t->p[0][0], t->p[1][0], t->p[2][0]}), tx1 = std::max({t->p[0][0], t->p[1][0], t->p[2][0]});
					const float ty0 = std::min({t->p[0][1], t->p[1][1], t->p[2][1]}), ty1 = std::max({t->p[0][1], t->p[1][1], t->p[2][1]});
					if (tx1 >= x0 && tx0 <= x0 + kPicture && ty1 >= y1 - kPicture && ty0 <= y1)
						here.push_back(t);
				}
				for (const bool sea : {false, true})
				{
					const int side = sea ? kSeaSide : kLandSide;
					const float metres = kPicture / (2 * side);
					texture::Image big(2 * side, 2 * side, sea ? kNoSea : kOpenSea);
					if (game)
					{
						// (the game's own picture there, the new land over it)
						texture::Image base;
						if (!GamePicture(files, std::format("{}_{}_{}", sea ? "minimap_sea" : "minimap", row, column), base, error))
							return false;
						for (int y = 0; y < big.height; ++y)
							for (int x = 0; x < big.width; ++x)
								big.At(x, y) = base.At(std::min(base.width - 1, x * base.width / big.width), std::min(base.height - 1, y * base.height / big.height));
					}
					// the box is ours: the open sea, then the land and the water in it
					for (int y = 0; y < big.height; ++y)
						for (int x = 0; x < big.width; ++x)
						{
							const float px = x0 + (x + 0.5f) * metres, py = y1 - (y + 0.5f) * metres;
							if (px >= box.x0 && px < box.x1 && py >= box.y0 && py < box.y1)
								big.At(x, y) = sea ? kNoSea : kOpenSea;
						}
					Draw(big, x0, y1, metres, here, sea);
					const std::string picture = std::format("{}_{}_{}_{}", sea ? "minimap_sea" : "minimap", name, row, column);
					if (!WritePicture(out, picture, Halve(big), sea ? texture::Format::Bc3 : texture::Format::Bc1, error))
						return false;
				}
				result.pictures.push_back({row, column});
			}
		// the open sea's (the squares of the widened grid nothing has a picture for)
		return WritePicture(out, std::format("minimap_{}_sea", name), texture::Image(64, 64, kOpenSea), texture::Format::Bc1, error) &&
		       WritePicture(out, std::format("minimap_sea_{}_sea", name), texture::Image(64, 64, kNoSea), texture::Format::Bc3, error);
	}
}
