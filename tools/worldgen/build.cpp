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
#include <optional>
#include <set>
#include <unordered_map>

#include <nlohmann/json.hpp>

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
#include "powerlines.hpp"
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
		constexpr uint8_t kGrass = 47, kSand = 19, kConcrete = 1, kDirt = 43, kGrassShort = 48, kLeaves = 52;
		// procedural.meta's procTagTable: what the game grows round the player on a material (research/phase0.md §63)
		constexpr uint16_t kForestOvergrown = 157 << 8, kMeadowBrown = 161 << 8, kLushShort = 148 << 8;

		float Smooth(float t)
		{
			t = std::clamp(t, 0.0f, 1.0f);
			return t * t * (3 - 2 * t);
		}

		float Dist(const P2& a, const P2& b) { return std::hypot(b[0] - a[0], b[1] - a[1]); }

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

		// A ground texture made neutral (a tinted layer: its colour mostly the tint palette's, the texture its pattern): a
		// third of its own colour kept, the rest its grey, all brightened so that the mean is `mean` (each channel scaled
		// apart had a green grass texture's bits of earth pink).
		void Neutral(texture::Image& image, float mean)
		{
			double sum = 0;
			for (auto& p : image.pixels)
			{
				const float grey = 0.299f * p[0] + 0.587f * p[1] + 0.114f * p[2];
				for (int c = 0; c < 3; ++c)
					p[c] = static_cast<uint8_t>(std::clamp(std::lround(grey + 0.35f * (p[c] - grey)), 0L, 255L));
				sum += grey;
			}
			const double m = std::max(1.0, sum / std::max<size_t>(1, image.pixels.size()));
			for (auto& p : image.pixels)
				for (int c = 0; c < 3; ++c)
					p[c] = static_cast<uint8_t>(std::clamp(std::lround(p[c] * mean / m), 0L, 255L));
		}

		// 256 colours for a tint palette (bytes): k-means over the wanted colours (weighted by how often each 8-step colour
		// is wanted), and each 8-step colour's nearest of them in `nearest` ((r >> 3) << 10 | (g >> 3) << 5 | b >> 3)
		std::array<std::array<uint8_t, 3>, 256> Palette(const std::vector<std::array<float, 3>>& wanted, std::vector<uint8_t>& nearest)
		{
			std::vector<std::array<double, 4>> bins(32768, {0, 0, 0, 0}); // (sum of r, g, b; count)
			for (const auto& c : wanted)
			{
				auto& b = bins[(static_cast<int>(c[0]) >> 3) << 10 | (static_cast<int>(c[1]) >> 3) << 5 | (static_cast<int>(c[2]) >> 3)];
				b[0] += c[0], b[1] += c[1], b[2] += c[2], b[3] += 1;
			}
			std::vector<std::array<float, 4>> points; // (mean colour of a bin, its weight)
			for (const auto& b : bins)
				if (b[3] > 0)
					points.push_back({static_cast<float>(b[0] / b[3]), static_cast<float>(b[1] / b[3]), static_cast<float>(b[2] / b[3]), static_cast<float>(b[3])});
			std::sort(points.begin(), points.end(), [](const auto& a, const auto& b) { return a[3] > b[3]; });
			std::array<std::array<float, 3>, 256> centre{};
			for (int k = 0; k < 256; ++k)
				centre[k] = points.empty() ? std::array<float, 3>{40, 55, 30} : std::array<float, 3>{points[k % points.size()][0], points[k % points.size()][1], points[k % points.size()][2]};
			const auto distance = [](const auto& a, const auto& b) { return (a[0] - b[0]) * (a[0] - b[0]) + (a[1] - b[1]) * (a[1] - b[1]) + (a[2] - b[2]) * (a[2] - b[2]); };
			for (int pass = 0; pass < 16 && !points.empty(); ++pass)
			{
				std::array<std::array<double, 4>, 256> sum{};
				for (const auto& q : points)
				{
					int best = 0;
					for (int k = 1; k < 256; ++k)
						if (distance(q, centre[k]) < distance(q, centre[best]))
							best = k;
					sum[best][0] += q[0] * q[3], sum[best][1] += q[1] * q[3], sum[best][2] += q[2] * q[3], sum[best][3] += q[3];
				}
				for (int k = 0; k < 256; ++k)
					if (sum[k][3] > 0)
						centre[k] = {static_cast<float>(sum[k][0] / sum[k][3]), static_cast<float>(sum[k][1] / sum[k][3]), static_cast<float>(sum[k][2] / sum[k][3])};
			}
			nearest.assign(32768, 0);
			for (int i = 0; i < 32768; ++i)
			{
				const std::array<float, 3> c{((i >> 10) << 3) + 4.0f, (((i >> 5) & 31) << 3) + 4.0f, ((i & 31) << 3) + 4.0f};
				int best = 0;
				for (int k = 1; k < 256; ++k)
					if (distance(c, centre[k]) < distance(c, centre[best]))
						best = k;
				nearest[i] = static_cast<uint8_t>(best);
			}
			std::array<std::array<uint8_t, 3>, 256> out{};
			for (int k = 0; k < 256; ++k)
				for (int c = 0; c < 3; ++c)
					out[k][c] = static_cast<uint8_t>(std::clamp(std::lround(centre[k][c]), 0L, 255L));
			return out;
		}

		// What the aerial pictures show (tools/worldgen/photo.py): trees.bin (every tree), classes.bin (each metre's cover),
		// cover.bin (each 4 m cell's class and its ground's colour).
		bool ReadPhoto(const std::filesystem::path& folder, std::vector<trees::Seen>& seen, raster::Grid<uint8_t>& classes,
		    raster::Grid<std::array<uint8_t, 4>>& cover, std::string& error)
		{
			{
				std::ifstream in(folder / "trees.bin", std::ios::binary);
				char magic[4]{};
				uint32_t version = 0, count = 0;
				in.read(magic, 4);
				in.read(reinterpret_cast<char*>(&version), 4);
				in.read(reinterpret_cast<char*>(&count), 4);
				if (!in || std::memcmp(magic, "WBTR", 4) != 0 || version != 1)
					return error = "not a trees file: " + (folder / "trees.bin").string(), false;
				seen.resize(count);
				for (auto& t : seen)
				{
					float v[4]{};
					uint8_t kind[4]{};
					in.read(reinterpret_cast<char*>(v), sizeof(v));
					in.read(reinterpret_cast<char*>(kind), sizeof(kind));
					t = {v[0], v[1], v[2], v[3], kind[0]};
				}
				if (!in)
					return error = "a trees file cut short: " + (folder / "trees.bin").string(), false;
			}
			const auto grid = [&](const char* file, const char* tag, auto& g) {
				std::ifstream in(folder / file, std::ios::binary);
				char magic[4]{};
				uint32_t version = 0, nx = 0, ny = 0;
				in.read(magic, 4);
				in.read(reinterpret_cast<char*>(&version), 4);
				in.read(reinterpret_cast<char*>(&g.x0), 4);
				in.read(reinterpret_cast<char*>(&g.y0), 4);
				in.read(reinterpret_cast<char*>(&g.cell), 4);
				in.read(reinterpret_cast<char*>(&nx), 4);
				in.read(reinterpret_cast<char*>(&ny), 4);
				if (!in || std::memcmp(magic, tag, 4) != 0 || version != 1 || g.cell <= 0 || !nx || !ny || nx > 65536 || ny > 65536)
					return error = std::string("not a photo.py ") + file + ": " + (folder / file).string(), false;
				g.nx = static_cast<int>(nx), g.ny = static_cast<int>(ny);
				g.v.resize(static_cast<size_t>(nx) * ny);
				if (!in.read(reinterpret_cast<char*>(g.v.data()), static_cast<std::streamsize>(g.v.size() * sizeof(g.v[0]))))
					return error = std::string("cut short: ") + (folder / file).string(), false;
				return true;
			};
			return grid("classes.bin", "WBCL", classes) && grid("cover.bin", "WBPC", cover);
		}

		// (a tinted ground layer's mean in each channel; the aerial pictures' colour of the ground times kPhotoGain is the
		// colour the game draws it with - the pictures are brighter than the ground's albedo: at 0.65 the game's fields came
		// out paler than the pictures beside them)
		constexpr float kNeutralMean = 200, kPhotoGain = 0.5f;
		// Pitched roofs' coverings (the game's textures: dictionary, texture, metres one covers): clay tiles, mid grey
		// shingles, light grey, dark (the mid grey's darker kin), brown
		struct Covering
		{
			const char *dictionary, *texture;
			float metres;
		};
		constexpr Covering kRoofCoverings[5] = {{"bh1_31_m2+hidr.ytd", "rsn_am_dc_roof_tile_01", 4.0f}, {"id2_21_ground.ytd", "id2_21_rsn_rk_xrefroofingb", 3.0f},
		    {"id2_21_ground.ytd", "id2_21_rsn_rk_xrefroofinga", 3.0f}, {"id2_21_ground.ytd", "id2_21_rsn_rk_xrefroofingb", 3.0f},
		    {"sc1_15_build7+hidr.ytd", "am_pp_bandstand_roofshingle_dr", 3.0f}};
		// (the woods' floor where the pictures see none, as a palette colour: NAIP's leaf litter in the shade about (95, 80, 60))
		constexpr std::array<float, 3> kForestFloor{95 * kPhotoGain / kNeutralMean * 255, 80 * kPhotoGain / kNeutralMean * 255, 60 * kPhotoGain / kNeutralMean * 255};
		// (and a town's lawns under its trees: NAIP's grass in the shade about (72, 86, 54))
		constexpr std::array<float, 3> kTownShade{72 * kPhotoGain / kNeutralMean * 255, 86 * kPhotoGain / kNeutralMean * 255, 54 * kPhotoGain / kNeutralMean * 255};

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
		    std::vector<P3>& colVertices, std::vector<std::array<uint32_t, 3>>& colTriangles, int coveringKey = -1, float coveringMetres = 3)
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
				if (coveringKey >= 0 && n[2] < 0.995f)
				{
					// a pitched roof's face: its covering's rows along the eaves (u along the level line, v down the slope)
					roads::Mesh& rm = meshes[static_cast<uint32_t>(coveringKey)];
					const float hl = std::max(1e-4f, std::hypot(n[0], n[1]));
					const P3 level{-n[1] / hl, n[0] / hl, 0};
					const P3 down{n[0] / hl * n[2], n[1] / hl * n[2], -hl}; // (in the face, pointing down the slope)
					const auto cu = [&](const P3& p) { return ((p[0] - cx) * level[0] + (p[1] - cy) * level[1]) / coveringMetres; };
					const auto cv = [&](const P3& p) { return ((p[0] - cx) * down[0] + (p[1] - cy) * down[1] + p[2] * down[2]) / coveringMetres; };
					const uint32_t i0 = add(rm, a, n, level, cu(a), cv(a)), i1 = add(rm, b, n, level, cu(b), cv(b)), i2 = add(rm, c, n, level, cu(c), cv(c));
					rm.indices.insert(rm.indices.end(), {i0, i1, i2});
					continue;
				}
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

		// A house's front porch (the street pictures' older streets: north Florida's frame houses most of all): along the wall
		// facing the street, from `a` to `b` (the footprint's anticlockwise order: outside on the right), a floor 0.45 m up
		// and 2.4 m deep, white posts at most 3 m apart, a shed roof in the house's covering from just under its eaves 0.55 m
		// down over the floor and a little past it; collision for the floor, the posts and the roof.
		struct Porch
		{
			bool has = false;
			P2 a{}, b{};
		};

		void AddPorch(const structures::Shape& s, const Porch& porch, uint32_t whiteKey, const facades::Style& white, uint32_t floorKey,
		    const facades::Style& floorStyle, int coveringKey, float coveringMetres, float cx, float cy, std::map<uint32_t, roads::Mesh>& meshes,
		    std::vector<P3>& colVertices, std::vector<std::array<uint32_t, 3>>& colTriangles)
		{
			const float len = std::hypot(porch.b[0] - porch.a[0], porch.b[1] - porch.a[1]);
			if (len < 3)
				return;
			const P2 t{(porch.b[0] - porch.a[0]) / len, (porch.b[1] - porch.a[1]) / len}, n{t[1], -t[0]};
			// (the eaves: the walls' lowest top round the outline)
			float eave = 1e30f;
			for (const auto& e : s.outline)
				eave = std::min(eave, static_cast<float>(s.vertices[e[0]][2]));
			if (eave > 1e29f || eave - s.base < 2.6f)
				return;
			constexpr float kDepth = 2.4f, kOver = 0.25f, kFloor = 0.45f, kPost = 0.08f;
			const float floorTop = s.base + kFloor, roofHigh = eave - 0.15f, roofLow = roofHigh - 0.55f;
			const auto at = [&](float along, float out, float z) { return P3{porch.a[0] + t[0] * along + n[0] * out, porch.a[1] + t[1] * along + n[1] * out, z}; };
			const auto vertex = [&](roads::Mesh& m, const P3& q, const P3& normal, const P3& tangent, float u, float v) {
				ydr::Vertex x;
				x.position = {q[0] - cx, q[1] - cy, q[2]};
				x.normal = normal;
				x.tangent = {tangent[0], tangent[1], tangent[2], 1};
				x.colour0 = {230, 12, 255, 255};
				x.uv[0] = {u, v};
				m.vertices.push_back(x);
				return static_cast<uint32_t>(m.vertices.size() - 1);
			};
			const auto collide = [&](const P3& q0, const P3& q1, const P3& q2) {
				const uint32_t k = static_cast<uint32_t>(colVertices.size());
				colVertices.insert(colVertices.end(), {q0, q1, q2});
				colTriangles.push_back({k, k + 1, k + 2});
			};
			// a quad as seen from its outside: bottom left, bottom right, top right, top left; white plaster from a plain part
			// of the house texture (between the floors' windows) unless given its own texture coordinates
			const auto quad = [&](uint32_t key, const P3& bl, const P3& br, const P3& tr, const P3& tl, const P3& normal, const P3& tangent,
			                      std::array<std::array<float, 2>, 4> uv, bool solid) {
				roads::Mesh& m = meshes[key];
				const uint32_t i0 = vertex(m, bl, normal, tangent, uv[0][0], uv[0][1]), i1 = vertex(m, br, normal, tangent, uv[1][0], uv[1][1]),
				               i2 = vertex(m, tr, normal, tangent, uv[2][0], uv[2][1]), i3 = vertex(m, tl, normal, tangent, uv[3][0], uv[3][1]);
				m.indices.insert(m.indices.end(), {i0, i1, i2, i0, i2, i3});
				if (solid)
					collide(bl, br, tr), collide(bl, tr, tl);
			};
			const std::array<std::array<float, 2>, 4> plain{{{0.03f, -0.45f}, {0.14f, -0.45f}, {0.14f, -0.6f}, {0.03f, -0.6f}}};
			const P3 up{0, 0, 1}, down{0, 0, -1}, out3{n[0], n[1], 0}, in3{-n[0], -n[1], 0}, along3{t[0], t[1], 0}, back3{-t[0], -t[1], 0};
			// the floor: its top (concrete, the gravel roofs' texture), its front and ends
			{
				const P3 a0 = at(0, 0, floorTop), b0 = at(len, 0, floorTop), b1 = at(len, kDepth, floorTop), a1 = at(0, kDepth, floorTop);
				const auto fu = [&](const P3& q) { return (q[0] - cx) / floorStyle.width * 4; };
				const auto fv = [&](const P3& q) { return -(q[1] - cy) / floorStyle.height * 4; };
				quad(floorKey, a1, b1, b0, a0, up, along3, {{{fu(a1), fv(a1)}, {fu(b1), fv(b1)}, {fu(b0), fv(b0)}, {fu(a0), fv(a0)}}}, true);
				const float low = s.base - 0.3f;
				quad(whiteKey, at(0, kDepth, low), at(len, kDepth, low), b1, a1, out3, along3, plain, true);
				quad(whiteKey, at(len, kDepth, low), at(len, 0, low), b0, b1, along3, in3, plain, true);
				quad(whiteKey, at(0, 0, low), at(0, kDepth, low), a1, a0, back3, out3, plain, true);
			}
			// the posts: square, white, from the floor to the roof's low side
			{
				const int spans = std::max(1, static_cast<int>(std::ceil((len - 0.4f) / 3.0f)));
				for (int k = 0; k <= spans; ++k)
				{
					const float along = 0.2f + (len - 0.4f) * static_cast<float>(k) / static_cast<float>(spans), out = kDepth - 0.2f;
					const float zt = roofLow + (roofHigh - roofLow) * (1 - out / (kDepth + kOver)) - 0.05f;
					const auto c = [&](float da, float dout, float z) { return at(along + da, out + dout, z); };
					quad(whiteKey, c(-kPost, kPost, floorTop), c(kPost, kPost, floorTop), c(kPost, kPost, zt), c(-kPost, kPost, zt), out3, along3, plain, true);
					quad(whiteKey, c(kPost, kPost, floorTop), c(kPost, -kPost, floorTop), c(kPost, -kPost, zt), c(kPost, kPost, zt), along3, in3, plain, true);
					quad(whiteKey, c(kPost, -kPost, floorTop), c(-kPost, -kPost, floorTop), c(-kPost, -kPost, zt), c(kPost, -kPost, zt), in3, back3, plain, true);
					quad(whiteKey, c(-kPost, -kPost, floorTop), c(-kPost, kPost, floorTop), c(-kPost, kPost, zt), c(-kPost, -kPost, zt), back3, out3, plain, true);
				}
			}
			// the roof: its top in the house's covering (rows along the eaves), a plaster underside and fascia
			{
				const float reach = kDepth + kOver;
				const P3 a0 = at(0, 0, roofHigh), b0 = at(len, 0, roofHigh), b1 = at(len, reach, roofLow), a1 = at(0, reach, roofLow);
				const float slope = std::hypot(reach, roofHigh - roofLow);
				const P3 normal{n[0] * (roofHigh - roofLow) / slope, n[1] * (roofHigh - roofLow) / slope, reach / slope};
				if (coveringKey >= 0)
				{
					const auto cu = [&](float along) { return along / coveringMetres; };
					const float vd = slope / coveringMetres;
					quad(static_cast<uint32_t>(coveringKey), a1, b1, b0, a0, normal, along3, {{{cu(0), vd}, {cu(len), vd}, {cu(len), 0}, {cu(0), 0}}}, true);
				}
				else
					quad(whiteKey, a1, b1, b0, a0, normal, along3, plain, true);
				quad(whiteKey, a0, b0, b1, a1, down, along3, plain, false);
				quad(whiteKey, at(0, reach, roofLow - 0.18f), at(len, reach, roofLow - 0.18f), b1, a1, out3, along3, plain, false);
			}
			(void)white;
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
		// what the aerial pictures show (tools/worldgen/photo.py, research/phase0.md §63): every tree, each metre's cover,
		// each 4 m cell's ground colour - the trees and the ground then as they are there
		std::vector<trees::Seen> photoTrees;
		raster::Grid<uint8_t> photoClass;
		raster::Grid<std::array<uint8_t, 4>> photoCover;
		if (!o.photo.empty())
		{
			if (!ReadPhoto(o.photo, photoTrees, photoClass, photoCover, error))
				return false;
			std::printf("photo: %zu trees, cover over %d x %d m\n", photoTrees.size(), photoClass.nx, photoClass.ny);
		}
		const bool photographed = !photoClass.v.empty();
		// (the colour the ground is tinted with round a point, as a palette colour: the pictures' colour of the ground they see
		// - grass, bare earth, paving: not the trees', not the water's - within 6 m, 10 m if none, times kPhotoGain over the
		// neutral layers' mean; nothing seen there - the woods' floor under their canopy: false)
		const auto groundColour = [&](float x, float y, std::array<float, 3>& out) {
			if (photoCover.v.empty())
				return false;
			const auto c = photoCover.Cell({x, y});
			for (int reach = 1; reach <= 2; ++reach)
			{
				float sum[3]{}, n = 0;
				for (int dj = -reach; dj <= reach; ++dj)
					for (int di = -reach; di <= reach; ++di)
					{
						if (!photoCover.In(c[0] + di, c[1] + dj))
							continue;
						const auto& p = photoCover.At(c[0] + di, c[1] + dj);
						if (p[0] < 3 || !(p[1] | p[2] | p[3]))
							continue;
						const float weight = 1.0f / (1 + di * di + dj * dj);
						for (int k = 0; k < 3; ++k)
							sum[k] += p[k + 1] * weight;
						n += weight;
					}
				if (n > 0)
				{
					for (int k = 0; k < 3; ++k)
						out[k] = std::clamp(sum[k] / n * kPhotoGain / kNeutralMean * 255.0f, 0.0f, 255.0f);
					// (a town's lawns in their growing season: NAIP flies Florida in winter, north Florida's lawns then dormant
					// and brown; the game's is always summer, as the street pictures of most of the year show them - a brownish
					// colour in developed land turned half way to a lawn's green of its brightness)
					if (landCoverAt)
						if (const uint8_t lc = landCoverAt(x, y); lc >= 21 && lc <= 24 && out[0] > out[2] + 10 && out[0] >= out[1] - 5)
						{
							const float l = (out[0] + out[1] + out[2]) / 3;
							const std::array<float, 3> lawn{l * 0.82f, l * 1.08f, l * 0.6f};
							for (int k = 0; k < 3; ++k)
								out[k] = std::clamp(out[k] + (lawn[k] - out[k]) * 0.55f, 0.0f, 255.0f);
						}
					return true;
				}
			}
			return false;
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
		// sidewalks where the map says; where it says nothing, by how built up the land along the road is (NLCD): the street
		// pictures' suburbs and country roads mostly have none - a verge of grass to the ditch -, the towns' middles and their
		// bigger streets have them (developed medium and high intensity for any street, low intensity too for tertiary and up)
		if (landCoverAt)
		{
			size_t without = 0;
			for (auto& r : near.roads)
			{
				if (r.sidewalkTagged || !r.sidewalks || r.points.size() < 2)
					continue;
				size_t built = 0, samples = 0;
				const bool big = r.kind.starts_with("primary") || r.kind.starts_with("secondary") || r.kind.starts_with("tertiary");
				for (size_t k = 0; k < r.points.size(); k += std::max<size_t>(1, r.points.size() / 8))
				{
					const uint8_t lc = landCoverAt(r.points[k][0], r.points[k][1]);
					built += lc == 23 || lc == 24 || (big && lc == 22);
					++samples;
				}
				if (built * 2 < samples)
					r.sidewalks = false, ++without;
			}
			std::printf("roads: %zu without sidewalks (the land along them not built up)\n", without);
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
			roads::Geometry(network, surfaces, planted, ground);
			// (the railways; a level crossing under the road's surface)
			const roads::Asphalt crossing(surfaces.collision, 0.5f);
			roads::Rails(w.rails, ground, [&](float x, float y) { return crossing.Within(x, y, 0.5f); }, surfaces);
			if (!w.rails.empty())
				std::printf("railways: %zu lines\n", w.rails.size());
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
		std::vector<int> shapeStyle, shapeRoof; // (shapeRoof: a pitched roof's covering, kRoofCoverings; -1 flat)
		std::vector<Porch> shapePorch;          // (a house's front porch, AddPorch)
		std::vector<P2> shapeCentre;
		facades::Set facadeSet;
		std::vector<world::Building> found; // (the buildings the scan saw that the map lacks)
		std::vector<std::vector<P2>> mapped; // (the footprints of those built, for the game's map)
		std::vector<buildings::Placement> placed;
		size_t unmatched = 0;
		// Without a scan: the game's models where one fits a footprint (shops, offices, apartments...), but houses of our own
		// (hip roofs, by the footprint: the game's Los Santos houses on Florida's lots looked nothing like them), and our own
		// flat-roofed blocks where no model fits (the footprints were left empty: a town's streets with every third house
		// missing)
		std::vector<const world::Building*> ownWithout;
		// (the streets' segments by 50 m cell: the side of a house that faces one)
		std::unordered_map<int64_t, std::vector<std::pair<P2, P2>>> streetCells;
		const auto StreetKey = [](float x, float y) {
			return static_cast<int64_t>(std::floor(x / 50)) << 32 | static_cast<uint32_t>(static_cast<int32_t>(std::floor(y / 50)));
		};
		for (const auto& e : network.edges)
			if (!roads::Minor(e))
				for (size_t k = 0; k + 1 < e.points.size(); ++k)
					streetCells[StreetKey((e.points[k][0] + e.points[k + 1][0]) / 2, (e.points[k][1] + e.points[k + 1][1]) / 2)].push_back({e.points[k], e.points[k + 1]});
		// (the walls the street pictures saw: by the building's place in the world's list)
		std::map<size_t, facades::Seen> seenWalls;
		if (!o.facades.empty())
		{
			std::ifstream in(o.facades);
			const nlohmann::json j = nlohmann::json::parse(in, nullptr, false);
			if (j.is_discarded() || !j.contains("buildings"))
				return error = "cannot read " + o.facades.string(), false;
			for (const auto& e : j["buildings"])
			{
				facades::Seen s;
				for (int c = 0; c < 3; ++c)
					s.colour[c] = static_cast<uint8_t>(std::clamp(e["colour"][c].get<int>(), 0, 255));
				s.brick = e["brick"].get<float>();
				seenWalls[e["building"].get<size_t>()] = s;
			}
			std::printf("buildings: %zu seen in the street pictures\n", seenWalls.size());
		}
		if (o.buildings && !scanned)
		{
			std::optional<buildings::Matcher> matcher;
			if (!catalog.empty())
				matcher.emplace(catalog);
			for (const auto& b : w.buildings)
			{
				if (b.footprint.size() < 3)
					continue;
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
				const double area = std::fabs(polygon::Area(b.footprint));
				// (and the low-rise apartments and townhouse rows: Florida's are pitched-roofed, two or three storeys)
				const bool house = b.kind == "house" || b.kind == "detached" || b.kind == "residential" || b.kind == "semidetached_house" ||
				                   b.kind == "bungalow" || b.kind == "terrace" || b.kind == "cabin" || b.kind == "farm" ||
				                   ((b.kind == "yes" || b.kind.empty()) && area >= 50 && area <= 350) ||
				                   ((b.kind == "apartments" || b.kind == "dormitory") && b.height <= 15 && area <= 3000);
				buildings::Placement p;
				if (!house && matcher && matcher->Pick(b.footprint, b.height, base, b.kind, p))
				{
					placed.push_back(p);
					mapped.push_back(b.footprint);
					raster::ForPolygon(floor, {b.footprint}, [&](int i, int j) { floor.At(i, j) = std::min(floor.At(i, j), base + 0.15f); });
					continue;
				}
				unmatched += !house;
				ownWithout.push_back(&b);
			}
			if (matcher)
			{
				// (which models went where, for checking)
				std::map<std::string, int> count;
				for (const auto& p : placed)
					++count[p.item->name];
				std::ofstream list(o.out.parent_path() / (o.name + "_buildings.txt"));
				for (const auto& [name, n] : count)
					list << n << ' ' << name << '\n';
				std::printf("buildings: %zu of the game's placed, %zu without a model that fits (%zu models to choose from)\n", placed.size(), unmatched,
				    matcher->Candidates());
			}
		}
		if (o.buildings && (scanned || !ownWithout.empty()))
		{
			if (scanned)
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
			facadeSet = facades::Make(o.name + "_f_", 1024, w.projection.lat0);
			structures::Options so;
			so.gableShare = w.projection.lat0 >= 28.8 ? 0.5f : w.projection.lat0 >= 27.0 ? 0.3f : 0.1f;
			structures::Stats ss;
			std::vector<const world::Building*> all;
			if (scanned)
			{
				for (const auto& b : w.buildings)
					all.push_back(&b);
				for (const auto& b : found)
					all.push_back(&b);
			}
			else
				all = ownWithout;
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
						if (scanned)
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
								made[i].ok = structures::Make(b, scanned ? &own : nullptr, ground, so, made[i].s, partStats[wk]);
								made[i].c = c;
							}
					});
				for (auto& worker : workers)
					worker.join();
			}
			for (const auto& p : partStats)
			{
				ss.measured += p.measured, ss.flat += p.flat, ss.shaped += p.shaped, ss.mapHeight += p.mapHeight, ss.skipped += p.skipped;
				ss.pitched += p.pitched;
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
				const bool listed = all[i] >= w.buildings.data() && all[i] < w.buildings.data() + w.buildings.size();
				const auto seenIt = listed ? seenWalls.find(static_cast<size_t>(all[i] - w.buildings.data())) : seenWalls.end();
				shapeStyle.push_back(facades::Pick(facadeSet, b.kind, made[i].s.height, area, seed, w.projection.lat0,
				    seenIt != seenWalls.end() ? &seenIt->second : nullptr));
				// (a pitched roof's covering by the aerial pictures' colour over the footprint: tiles, light or dark shingles)
				int roof = -1;
				if (made[i].s.pitched)
				{
					roof = 1;
					if (!photoCover.v.empty())
						if (const auto pc = photoCover.Cell(c); photoCover.In(pc[0], pc[1]))
						{
							const auto& px = photoCover.At(pc[0], pc[1]);
							const float r = px[1], g = px[2], bl = px[3], lum = (r + g + bl) / 3;
							roof = r > g + 18 && r > bl + 35 ? 0 : lum > 165 ? 2 : lum < 95 ? 3 : r > bl + 22 ? 4 : 1;
						}
				}
				shapeRoof.push_back(roof);
				// (a front porch on some of the houses - north Florida's a third, the south's few: on the outline's edge facing
				// the nearest street within 40 m, its middle 85 %, the edge 7 m long or more)
				Porch porch;
				{
					uint32_t ps = static_cast<uint32_t>(std::lround(c[0] * 5)) * 2654435761u ^ static_cast<uint32_t>(std::lround(c[1] * 5)) * 40503u;
					ps ^= ps >> 13, ps *= 0x5bd1e995u, ps ^= ps >> 15;
					const float share = w.projection.lat0 >= 28.8 ? 0.35f : w.projection.lat0 >= 27.0 ? 0.2f : 0.08f;
					if (made[i].s.pitched && static_cast<float>(ps % 1000) < share * 1000)
					{
						P2 street{};
						float best = 40;
						for (int dj = -1; dj <= 1; ++dj)
							for (int di = -1; di <= 1; ++di)
								if (const auto it = streetCells.find(StreetKey(c[0] + di * 50.0f, c[1] + dj * 50.0f)); it != streetCells.end())
									for (const auto& [a, q] : it->second)
									{
										const P2 d{q[0] - a[0], q[1] - a[1]};
										const float l2 = d[0] * d[0] + d[1] * d[1];
										const float tt = l2 > 0 ? std::clamp(((c[0] - a[0]) * d[0] + (c[1] - a[1]) * d[1]) / l2, 0.0f, 1.0f) : 0.0f;
										const P2 pt{a[0] + d[0] * tt, a[1] + d[1] * tt};
										if (const float dist = std::hypot(pt[0] - c[0], pt[1] - c[1]); dist < best)
											best = dist, street = pt;
									}
						if (best < 40)
						{
							const P2 toStreet{(street[0] - c[0]) / std::max(best, 0.01f), (street[1] - c[1]) / std::max(best, 0.01f)};
							const auto& f = b.footprint;
							float facing = 0.7f;
							for (size_t e = 0; e < f.size(); ++e)
							{
								const P2 &fa = f[e], &fb = f[(e + 1) % f.size()];
								const float l = std::hypot(fb[0] - fa[0], fb[1] - fa[1]);
								if (l < 7)
									continue;
								const P2 nrm{(fb[1] - fa[1]) / l, -(fb[0] - fa[0]) / l};
								if (const float dot = nrm[0] * toStreet[0] + nrm[1] * toStreet[1]; dot > facing)
								{
									facing = dot;
									const float trim = 0.075f * l;
									porch.has = true;
									porch.a = {fa[0] + (fb[0] - fa[0]) / l * trim, fa[1] + (fb[1] - fa[1]) / l * trim};
									porch.b = {fb[0] - (fb[0] - fa[0]) / l * trim, fb[1] - (fb[1] - fa[1]) / l * trim};
								}
							}
						}
					}
				}
				shapePorch.push_back(porch);
				shapeCentre.push_back(c);
				shapes.push_back(std::move(made[i].s));
			}
			std::printf("buildings: %zu of our own (%zu measured: %zu flat, %zu shaped; %zu hip roofs; %zu at the map's height; %zu skipped), %zu roof "
			            "triangles\n",
			    shapes.size(), ss.measured, ss.flat, ss.shaped, ss.pitched, ss.mapHeight, ss.skipped, ss.triangles);
		}
		stats.buildings = shapes.size() + placed.size();
		took("buildings");
		// street furniture and trees: off the asphalt, outside buildings
		std::vector<props::Prop> furniture;
		roads::Mesh wires; // (the power lines', powerlines.hpp)
		std::vector<mapfiles::CarGen> parked; // (parked cars: the game's car generators)
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
			props::Place(near, network, surfaces.signals, open, ground, {bx0, by0, bx1, by1}, furniture, !scanned && photoTrees.empty());
			std::printf("props: %zu (palms, street lights, signals)\n", furniture.size());
			if (!photoTrees.empty() && !scanned)
			{
				// (every tree the aerial pictures show, at its height and crown - a crown over a road shrunk to the room there,
				// the tree left out where there is too little: the woods' roads are open in the pictures)
				raster::Grid<uint8_t> offRoad;
				offRoad.Init(gx0, gy0, gx1, gy1, 2.0f, 1);
				for (const auto& e : network.edges)
					raster::DrawLine(offRoad, e.points, e.HalfWidth(), uint8_t{0});
				const auto toRoad = raster::DistanceTo(offRoad, 1);
				std::vector<trees::Seen> kept;
				kept.reserve(photoTrees.size());
				const auto roomAt = [&](float x, float y) {
					const auto c = toRoad.Cell({x, y});
					return toRoad.In(c[0], c[1]) ? toRoad.At(c[0], c[1]) : 1e9f;
				};
				constexpr float kTurn = 3.14159265f / 6;
				size_t moved = 0;
				for (trees::Seen t : photoTrees)
				{
					// (a trunk the pictures put on a road or a roof - a crown's brightest point is often over one - to the
					// nearest open ground under its crown: Tallahassee's yards had lost most of their oaks)
					if (roomAt(t.x, t.y) < 1.5f || !open(t.x, t.y))
					{
						bool found = false;
						for (float d = 1.5f; d <= std::min(t.crown, 6.0f) + 0.01f && !found; d += 1.5f)
							for (int k = 0; k < 12 && !found; ++k)
							{
								const float x = t.x + d * std::cos(k * kTurn), y = t.y + d * std::sin(k * kTurn);
								if (roomAt(x, y) >= 1.5f && open(x, y))
									t.x = x, t.y = y, found = true;
							}
						if (!found)
							continue;
						++moved;
					}
					// (a crown over a road shrunk to the room there where the pictures see the road open - the woods' roads -,
					// not where they see the canopy close over it, as over Tallahassee's streets)
					const float room = roomAt(t.x, t.y);
					if (t.crown > room / 1.15f)
					{
						bool openRoad = false;
						for (int k = 0; k < 12 && !openRoad; k += 1)
						{
							const float x = t.x + t.crown * std::cos(k * kTurn), y = t.y + t.crown * std::sin(k * kTurn);
							if (roomAt(x, y) > 0)
								continue; // (not on a road)
							const auto pc = photoClass.Cell({x, y});
							openRoad = photoClass.In(pc[0], pc[1]) && photoClass.At(pc[0], pc[1]) != 2;
						}
						if (openRoad)
							t.crown = room / 1.15f;
					}
					kept.push_back(t);
				}
				std::printf("trees from the aerial pictures: %zu moved off roads and roofs\n", moved);
				trees::Stats ts;
				trees::Photo(kept, landCoverAt, w.projection.lat0, open, ground, {bx0, by0, bx1, by1}, furniture, ts);
				std::printf("trees from the aerial pictures: %zu palms, %zu others\n", ts.palms, ts.broadleaf);
			}
			else if (scanned)
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
				    loader::convert::Joaat("prop_traffic_01b"), loader::convert::Joaat("prop_sign_road_01a")};
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
				// power lines along the streets and roads (powerlines.hpp): a pole where nothing stands - off the asphalt as a
				// post is, 3 m from any other post -, the trees at its foot left out (the line's own clearing)
				constexpr float kPostCell = 4;
				std::unordered_map<int64_t, std::vector<P2>> postAt;
				const auto postKey = [&](float x, float y) {
					return key(static_cast<int>(std::floor(x / kPostCell)), static_cast<int>(std::floor(y / kPostCell)));
				};
				for (const auto& p : furniture)
					if (std::find(std::begin(posts), std::end(posts), p.archetype) != std::end(posts))
						postAt[postKey(p.position[0], p.position[1])].push_back({p.position[0], p.position[1]});
				const auto nearPost = [&](float x, float y, float r) {
					for (int dj = -1; dj <= 1; ++dj)
						for (int di = -1; di <= 1; ++di)
							if (const auto it = postAt.find(postKey(x + di * kPostCell, y + dj * kPostCell)); it != postAt.end())
								for (const P2& q : it->second)
									if (std::hypot(q[0] - x, q[1] - y) < r)
										return true;
					return false;
				};
				const auto poleClear = [&](float x, float y) {
					const float r = atCorner(x, y) ? roads::kCornerPostClear : roads::kPostClear;
					return open(x, y) && !asphalt.Within(x, y, r) && !nearPost(x, y, 3);
				};
				std::vector<props::Prop> poles;
				powerlines::Stats ps;
				powerlines::Place(network, poleClear, ground, {bx0, by0, bx1, by1}, poles, wires, ps);
				postAt.clear();
				for (const auto& p : poles)
					postAt[postKey(p.position[0], p.position[1])].push_back({p.position[0], p.position[1]});
				const size_t before = furniture.size();
				std::erase_if(furniture, [&](const props::Prop& p) {
					return std::find(std::begin(posts), std::end(posts), p.archetype) == std::end(posts) && nearPost(p.position[0], p.position[1], 2.5f);
				});
				std::printf("power lines: %zu poles, %zu spans, %zu wires (%zu trees by the poles left out)\n", ps.poles, ps.spans, ps.wires,
				    before - furniture.size());
				furniture.insert(furniture.end(), poles.begin(), poles.end());
			}
			{
				// (what stands where, for checking: archetype hash, x, y, z, scale)
				std::ofstream list(o.out.parent_path() / (o.name + "_props.txt"));
				for (const auto& prop : furniture)
					list << std::format("{:08x} {:.1f} {:.1f} {:.1f} {:.2f}\n", prop.archetype, prop.position[0], prop.position[1], prop.position[2], prop.scale);
			}
			// Parked cars (research/phase0.md §63): the game makes one of the area's cars at a car generator when the player
			// comes near. On houses' driveways - between a house and the street it faces, past the sidewalk, to one side,
			// most nose in, three in four houses, now and then a second car -, and in car parks: stalls in rows along the
			// park's long side, back to back with an aisle between, nearly half taken. A car where it would touch a road,
			// a building or a tree's trunk is left out.
			{
				const auto free = [&](float x, float y, const P2& dir) {
					for (const float along : {-2.4f, 0.0f, 2.4f})
						for (const float side : {-0.9f, 0.9f})
						{
							const float px = x + dir[0] * along - dir[1] * side, py = y + dir[1] * along + dir[0] * side;
							if (!open(px, py))
								return false;
						}
					return true;
				};
				// (the trees' trunks: no car on one)
				std::unordered_map<int64_t, std::vector<P2>> trunks;
				const auto key8 = [](float x, float y) { return static_cast<int64_t>(std::floor(x / 8)) << 32 | static_cast<uint32_t>(static_cast<int32_t>(std::floor(y / 8))); };
				for (const auto& p : furniture)
					trunks[key8(p.position[0], p.position[1])].push_back({p.position[0], p.position[1]});
				const auto clearOfTrunks = [&](float x, float y) {
					for (int dj = -1; dj <= 1; ++dj)
						for (int di = -1; di <= 1; ++di)
							if (const auto it = trunks.find(key8(x + di * 8.0f, y + dj * 8.0f)); it != trunks.end())
								for (const P2& t : it->second)
									if (Dist(t, P2{x, y}) < 3.2f)
										return false;
					return true;
				};
				const auto place = [&](float x, float y, const P2& dir) {
					if (x < bx0 || y < by0 || x >= bx1 || y >= by1 || !free(x, y, dir) || !clearOfTrunks(x, y))
						return false;
					mapfiles::CarGen c;
					c.position = {x, y, ground(x, y) + 0.05f};
					c.direction = {dir[0], dir[1]};
					parked.push_back(c);
					return true;
				};
				// (the streets a house faces: residential and the small kinds; their segments by 50 m cell)
				std::unordered_map<int64_t, std::vector<std::pair<int, int>>> segments; // (edge, point index)
				const auto key50 = [](float x, float y) { return static_cast<int64_t>(std::floor(x / 50)) << 32 | static_cast<uint32_t>(static_cast<int32_t>(std::floor(y / 50))); };
				for (size_t ei = 0; ei < network.edges.size(); ++ei)
				{
					const auto& e = network.edges[ei];
					if (!(e.kind == "residential" || e.kind == "living_street" || e.kind == "unclassified" || e.kind == "tertiary" || e.kind == "service" ||
					        e.kind == "secondary"))
						continue;
					for (size_t k = 0; k + 1 < e.points.size(); ++k)
					{
						const P2 m{(e.points[k][0] + e.points[k + 1][0]) / 2, (e.points[k][1] + e.points[k + 1][1]) / 2};
						segments[key50(m[0], m[1])].push_back({static_cast<int>(ei), static_cast<int>(k)});
					}
				}
				size_t driveways = 0, stalls = 0, shrubs = 0, mailboxes = 0;
				// (the yards' things, as the street pictures show them: shrubs along a house's front - foundation plantings -, and
				// where the street has no sidewalk a mailbox on its post at the asphalt's edge by the driveway)
				static const char* kShrubs[] = {"prop_bush_med_01", "prop_bush_med_02", "prop_bush_med_05", "prop_bush_neat_02", "prop_bush_neat_04",
				    "prop_bush_med_07"};
				static const char* kMailboxes[] = {"prop_letterbox_01", "prop_letterbox_02", "prop_letterbox_03", "prop_letterbox_04"};
				for (size_t bi = 0; bi < w.buildings.size(); ++bi)
				{
					const auto& b = w.buildings[bi];
					if (b.footprint.size() < 3)
						continue;
					double area2 = 0;
					P2 c{0, 0};
					for (size_t k = 0; k < b.footprint.size(); ++k)
					{
						const P2 &p = b.footprint[k], &q = b.footprint[(k + 1) % b.footprint.size()];
						area2 += static_cast<double>(p[0]) * q[1] - static_cast<double>(q[0]) * p[1];
						c[0] += p[0], c[1] += p[1];
					}
					c[0] /= b.footprint.size(), c[1] /= b.footprint.size();
					const float area = static_cast<float>(std::fabs(area2) / 2);
					const bool house = b.kind == "house" || b.kind == "detached" || b.kind == "semidetached_house" || b.kind == "bungalow" ||
					                   ((b.kind == "yes" || b.kind == "residential") && area > 60 && area < 350);
					if (!house || area < 40 || area > 600 || c[0] < bx0 || c[1] < by0 || c[0] >= bx1 || c[1] >= by1)
						continue;
					uint32_t seed = static_cast<uint32_t>(bi) * 2654435761u;
					seed ^= seed >> 15, seed *= 0x2c1b3c6du, seed ^= seed >> 12;
					if (seed % 100 >= 75)
						continue;
					// (the nearest such street within 45 m)
					float best = 45;
					P2 nearest{}, along{};
					float half = 0;
					bool walks = false; // (the street's sidewalks)
					for (int dj = -1; dj <= 1; ++dj)
						for (int di = -1; di <= 1; ++di)
							if (const auto it = segments.find(key50(c[0] + di * 50.0f, c[1] + dj * 50.0f)); it != segments.end())
								for (const auto& [ei, k] : it->second)
								{
									const auto& e = network.edges[ei];
									const P2 &a = e.points[k], &q = e.points[k + 1];
									const P2 d{q[0] - a[0], q[1] - a[1]};
									const float l2 = d[0] * d[0] + d[1] * d[1];
									const float t = l2 > 0 ? std::clamp(((c[0] - a[0]) * d[0] + (c[1] - a[1]) * d[1]) / l2, 0.0f, 1.0f) : 0.0f;
									const P2 p{a[0] + d[0] * t, a[1] + d[1] * t};
									if (const float dist = Dist(p, c); dist < best)
									{
										const float l = std::sqrt(l2);
										best = dist, nearest = p, along = {d[0] / l, d[1] / l}, half = e.HalfWidth(), walks = e.sidewalks;
									}
								}
					if (best >= 45)
						continue;
					const P2 u{(c[0] - nearest[0]) / best, (c[1] - nearest[1]) / best}; // (from the street to the house)
					const float radius = std::sqrt(area / 3.14159f);
					const float room = best - half - 0.8f * radius;
					if (room < 8.5f)
						continue;
					// (past the sidewalk and verge: the car's middle 2.5 + 2.8 m from the carriageway's edge, to one side)
					const float inward = half + 5.3f + std::min(room - 8.5f, 3.0f) * ((seed >> 8) % 100) / 100.0f;
					const float side = (((seed >> 16) & 1) ? 1.0f : -1.0f) * radius * (0.35f + 0.25f * ((seed >> 20) % 100) / 100.0f);
					const float x = nearest[0] + u[0] * inward + along[0] * side, y = nearest[1] + u[1] * inward + along[1] * side;
					const P2 dir = (seed >> 24) % 100 < 70 ? u : P2{-u[0], -u[1]};
					// the house's front: the outline's edge facing the street most (outward normal of an anticlockwise outline:
					// the edge's right), shrubs 0.9 m out every 2.5-3.5 m, the door's middle 1.6 m left free
					{
						const auto& f = b.footprint;
						float facing = 0.6f;
						size_t front = f.size();
						for (size_t k = 0; k < f.size(); ++k)
						{
							const P2 &a = f[k], &q = f[(k + 1) % f.size()];
							const float l = Dist(a, q);
							if (l < 6)
								continue;
							const P2 n{(q[1] - a[1]) / l, -(q[0] - a[0]) / l};
							if (const float dot = -(n[0] * u[0] + n[1] * u[1]); dot > facing)
								facing = dot, front = k;
						}
						if (front < f.size())
						{
							const P2 &a = f[front], &q = f[(front + 1) % f.size()];
							const float l = Dist(a, q);
							const P2 t{(q[0] - a[0]) / l, (q[1] - a[1]) / l}, n{t[1], -t[0]};
							uint32_t r = seed;
							for (float at = 1.2f; at < l - 1.0f; at += 2.5f + static_cast<float>((r >> 3) % 100) / 100.0f)
							{
								r = r * 1664525u + 1013904223u;
								if (std::fabs(at - l / 2) < 0.8f)
									continue;
								const float sx = a[0] + t[0] * at + n[0] * 0.9f, sy = a[1] + t[1] * at + n[1] * 0.9f;
								if (sx < bx0 || sy < by0 || sx >= bx1 || sy >= by1 || !open(sx, sy) || Dist(P2{sx, sy}, P2{x, y}) < 3.5f)
									continue;
								furniture.push_back({loader::convert::Joaat(kShrubs[(r >> 16) % std::size(kShrubs)]), {sx, sy, ground(sx, sy) - 0.1f},
								    static_cast<float>((r >> 8) % 360) * 3.14159265f / 180, 120, 0.8f + static_cast<float>((r >> 20) % 30) / 100});
								++shrubs;
							}
						}
					}
					// (the mailbox: on the driveway's side, 1.1 m from the asphalt, facing the street - a letterbox's door is its
					// -y: towards the road)
					if (!walks && (seed >> 12) % 100 < 85)
					{
						const float mx = nearest[0] + u[0] * (half + 1.1f) + along[0] * (side + (side > 0 ? 1.6f : -1.6f));
						const float my = nearest[1] + u[1] * (half + 1.1f) + along[1] * (side + (side > 0 ? 1.6f : -1.6f));
						if (mx >= bx0 && my >= by0 && mx < bx1 && my < by1 && open(mx, my))
						{
							furniture.push_back({loader::convert::Joaat(kMailboxes[(seed >> 5) % std::size(kMailboxes)]), {mx, my, ground(mx, my)},
							    std::atan2(u[1], u[0]) + 3.14159265f / 2, 150});
							++mailboxes;
						}
					}
					if (place(x, y, dir))
					{
						++driveways;
						if ((seed >> 4) % 100 < 25) // (a second car beside it)
							driveways += place(x + along[0] * (side > 0 ? -2.9f : 2.9f), y + along[1] * (side > 0 ? -2.9f : 2.9f), dir);
					}
				}
				// car parks: their long side's direction (of their outline's edges, the longest's), rows of stalls 2.7 m
				// wide and 5.5 m deep back to back, a 7 m aisle between the pairs
				for (size_t ai = 0; ai < w.areas.size(); ++ai)
				{
					const auto& a = w.areas[ai];
					if (a.kind != "parking" || a.outer.empty() || a.outer[0].size() < 3)
						continue;
					const auto& ring = a.outer[0];
					P2 dir{1, 0};
					float longest = 0;
					for (size_t k = 0; k < ring.size(); ++k)
					{
						const P2 &p = ring[k], &q = ring[(k + 1) % ring.size()];
						if (const float l = Dist(p, q); l > longest)
							longest = l, dir = {(q[0] - p[0]) / l, (q[1] - p[1]) / l};
					}
					const P2 nrm{-dir[1], dir[0]};
					float s0 = 1e30f, s1 = -1e30f, t0 = 1e30f, t1 = -1e30f;
					for (const P2& p : ring)
					{
						const float s = p[0] * dir[0] + p[1] * dir[1], t = p[0] * nrm[0] + p[1] * nrm[1];
						s0 = std::min(s0, s), s1 = std::max(s1, s), t0 = std::min(t0, t), t1 = std::max(t1, t);
					}
					if ((s1 - s0) * (t1 - t0) > 400000) // (a huge one: not stalls everywhere)
						continue;
					const auto inside = [&](float x, float y) {
						bool in = false;
						for (size_t k = 0, l = ring.size() - 1; k < ring.size(); l = k++)
							if ((ring[k][1] > y) != (ring[l][1] > y) &&
							    x < (ring[l][0] - ring[k][0]) * (y - ring[k][1]) / (ring[l][1] - ring[k][1]) + ring[k][0])
								in = !in;
						return in;
					};
					uint32_t seed = static_cast<uint32_t>(ai) * 0x9E3779B1u;
					for (float t = t0 + 3.5f; t + 2.75f < t1; t += 18.0f) // (a pair of rows: 5.5 + 5.5 deep, 7 m aisle)
						for (const float row : {t + 2.75f, t + 8.25f})
							for (float s = s0 + 1.35f; s + 1.35f < s1; s += 2.7f)
							{
								seed ^= seed << 13, seed ^= seed >> 17, seed ^= seed << 5;
								const float x = dir[0] * s + nrm[0] * row, y = dir[1] * s + nrm[1] * row;
								if (seed % 100 >= 45 || !inside(x, y) || !inside(x + nrm[0] * 2.7f, y + nrm[1] * 2.7f) || !inside(x - nrm[0] * 2.7f, y - nrm[1] * 2.7f))
									continue;
								// (facing the aisle: the first row of a pair backs onto the second)
								const P2 face = row < t + 5 ? P2{-nrm[0], -nrm[1]} : nrm;
								stalls += place(x, y, face);
							}
				}
				std::printf("parked cars: %zu on driveways, %zu in car parks; %zu shrubs by houses, %zu mailboxes\n", driveways, stalls, shrubs, mailboxes);
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
		ydr::Material lawnMaterial, groundForest, riverMaterial, forestMaterial, photoGround;
		std::array<std::pair<std::string, std::string>, 5> photoKinds; // (the ground's kinds' textures: diffuse, normal)
		std::array<std::array<uint8_t, 3>, 256> photoPalette{};
		std::vector<uint8_t> photoNearest; // (a wanted colour's palette column: Palette)
		uint8_t photoUsual = 0;            // (the column of the woods' floor: where the pictures show no ground near)
		uint8_t photoTown = 0;             // (the column of a town's shaded lawn: the same in developed land)
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
		uint32_t coveringBase = 0; // (wallMaterials' index of the first pitched roof covering)
		std::array<std::string, std::size(kRoofCoverings)> coveringTextures; // (their textures' names in our dictionary)
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
		ydr::Material railTrackMaterial = asphalt, railBedMaterial = asphalt, railSteelMaterial = asphalt, cableMaterial;
		ydr::Material wornMaterial = asphalt; // (worn asphalt: the game's cracked road picture and its relief)
		ydr::Material patchDecal, potholeDecal, breakupDecal;
		{
			std::map<std::pair<std::string, std::string>, std::string> copied; // (dictionary, texture) -> our name
			// (treat: 1 a ground's texture, Flatten'ed - in the format it had; 2 also made Neutral, a tinted layer's)
			const auto use = [&](const char* dictionary, const char* texture, std::string& name, int treat = 0) {
				// (3: a roof covering 0.6 as bright - lit from above at noon the game's shingles came out near white)
				const char* suffix = treat == 3 ? "_dim" : treat == 2 ? "_neutral" : treat ? "_flat" : "";
				const auto key = std::make_pair(std::string(dictionary), std::string(texture) + suffix);
				if (const auto it = copied.find(key); it != copied.end())
					return name = it->second, true;
				loader::convert::DictionaryTexture t;
				name = std::format("{}_{}{}", o.name, texture, suffix);
				if (!CopyTexture(files, dictionary, texture, name, t, error))
					return false;
				if (treat)
				{
					texture::Image image;
					if (!GameImage(files, dictionary, texture, image, error))
						return false;
					if (treat == 3)
					{
						for (auto& p : image.pixels)
							for (int c = 0; c < 3; ++c)
								p[c] = static_cast<uint8_t>(p[c] * 0.6f);
					}
					else
						Flatten(image);
					if (treat == 2)
						Neutral(image, kNeutralMean);
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
					if (!use(groundLayers[k][0], groundLayers[k][1], d, 1) || !use(groundLayers[k][2], groundLayers[k][3], n))
						return false;
					ground3.Texture(loader::convert::Joaat(std::format("DiffuseTexture_layer{}", k)), d);
					ground3.Texture(loader::convert::Joaat(std::format("BumpTexture_layer{}", k)), n);
					if (!use(lawns[k][0], lawns[k][1], d, 1) || !use("ch1_09_arc007+hidr.ytd", "im_grass07_n", n))
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
						if (!use(litter[k][0], litter[k][1], d, 1) || !use(litter[k][2], litter[k][3], n))
							return false;
						forestMaterial.Texture(loader::convert::Joaat(std::format("DiffuseTexture_layer{}", k)), d);
						forestMaterial.Texture(loader::convert::Joaat(std::format("BumpTexture_layer{}", k)), n);
					}
				}
				// the forest floor (inland tiles: leaf litter in place of the beach's sand, mud in place of the wet sand)
				groundForest = ground3;
				if (!use("bh1_36_terrain+hi.ytd", "rsn_am_ground_dirt_leaflitter", d, 1) || !use("bh1_36_terrain+hi.ytd", "rsn_am_ground_dirt_leaflitter_n", n))
					return false;
				groundForest.Texture(loader::convert::Joaat("DiffuseTexture_layer2"), d);
				groundForest.Texture(loader::convert::Joaat("BumpTexture_layer2"), n);
				// (and under the lakes and rivers a mud bank's mud, not the beach's wet sand: through the water it had shown as a
				// bright sandy floor)
				if (!use("ch1_09_terrain+hi.ytd", "rn_tf_mudbank_003", d, 1) || !use("ch1_09_terrain+hi.ytd", "rn_tf_mudbank_002_n", n))
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
				// (the pitched roofs' coverings, when there are buildings of our own)
				if (!shapes.empty())
					for (size_t k = 0; k < std::size(kRoofCoverings); ++k)
						if (!use(kRoofCoverings[k].dictionary, kRoofCoverings[k].texture, coveringTextures[k], 3))
							return false;
				// the railways': the track (wooden sleepers on ballast, rails pictured), the ballast, the rails' steel
				if (!use("po1_04_bridge2+hidr.ytd", "rsn_et_railtrack001", d) || !use("po1_04+hi.ytd", "rsn_gg_ballast", n))
					return false;
				railTrackMaterial.Texture(kDiffuseTex, d);
				railBedMaterial.Texture(kDiffuseTex, n);
				shared.push_back(texture::Make(o.name + "_steel", texture::Image(4, 4, {96, 94, 92, 255}), texture::Format::Bc1));
				shared.push_back(texture::Make(o.name + "_steel_s", texture::Image(4, 4, {150, 150, 150, 255}), texture::Format::Bc1));
				railSteelMaterial.Texture(kDiffuseTex, o.name + "_steel");
				railSteelMaterial.Texture(kSpecularTex, o.name + "_steel_s");
				// the power lines' wires: the game's cables (a cable mesh's shader, its texture)
				if (!wires.indices.empty())
				{
					if (!GameMaterial(files, "ap1_01_b_cablemesh1050744_thvy.ydr", 0, cableMaterial, error) || !use("cablemesh.ytd", "cable", d))
						return false;
					cableMaterial.Texture(0x8D42F627, d); // (its "cable")
				}
				// The ground as the aerial pictures show it (research/phase0.md §63): the golf course's shader
				// (terrain_cb_w_4lyr_cm_tnt, bh1_43_golf_1a #6) - four layers mixed by a picture of their weights (each tile its
				// own, from the pictures' cover every 1.6 m), tinted by a palette each vertex picks its colour from. Each tile's
				// four of five kinds of ground - grass, dry grass, bare earth, paving, leaf litter - by what the pictures show
				// there, every one neutral and tinted in full with the pictures' colour of the ground round the vertex.
				if (photographed)
				{
					if (!GameMaterial(files, "bh1_43_golf_1a.ydr", 6, photoGround, error))
						return false;
					// (little standing water after rain, as on the lawns: the golf course's 0.2-0.3 mirrored the sky in pools
					// across the woods' floor)
					for (auto& [parameter, bytes] : photoGround.constants)
						if (parameter == loader::convert::Joaat("materialWetnessMultiplier"))
							for (size_t k = 0; k + 4 <= bytes.size(); k += 4)
							{
								float v;
								std::memcpy(&v, bytes.data() + k, 4);
								v *= 0.1f;
								std::memcpy(bytes.data() + k, &v, 4);
							}
					struct PhotoLayer
					{
						const char *dictionary, *diffuse, *normalDictionary, *normal;
						int treat;
						float tint;
					};
					// (every layer only a pattern - neutral, its colour the pictures' through the palette: a ploughed field white,
					// a pasture tan, a lawn green, the woods' floor brown; the kinds in kPhotoKind's order)
					const PhotoLayer kinds[5] = {{"ch1_06e_land+hi.ytd", "im_grass10b", "ch1_09_arc007+hidr.ytd", "im_grass07_n", 2, 1.0f},
					    {"ch1_05_terrain+hi.ytd", "cs_tf_rsn_redgrass1", "ch1_05_terrain+hi.ytd", "cs_tf_rsn_redgrass1_n", 2, 1.0f},
					    {"venicebeachground+hi.ytd", "nxg_rsn_os_beachsoftprints", "venicebeachground+hi.ytd", "nxg_rsn_os_beachsoftprints_n", 2, 1.0f},
					    {"sp1_03_ground+hidr.ytd", "im_concrete25", "paletoroadtemp+hi.ytd", "im_concrete25_n", 2, 1.0f},
					    {"bh1_36_terrain+hi.ytd", "rsn_am_ground_dirt_leaflitter", "bh1_36_terrain+hi.ytd", "rsn_am_ground_dirt_leaflitter_n", 2, 1.0f}};
					for (int k = 0; k < 5; ++k)
					{
						if (!use(kinds[k].dictionary, kinds[k].diffuse, d, kinds[k].treat) || !use(kinds[k].normalDictionary, kinds[k].normal, n))
							return false;
						photoKinds[k] = {d, n};
					}
					for (int k = 0; k < 4; ++k)
					{
						const float full = 1.0f;
						std::vector<uint8_t> bytes(4);
						std::memcpy(bytes.data(), &full, 4);
						photoGround.constants.emplace_back(loader::convert::Joaat(std::format("tintBlendLayer{}", k)), bytes);
					}
					photoGround.Texture(loader::convert::Joaat("TintPaletteTex"), o.name + "_pal");
					// The palette: 256 colours for the ground, k-means over the colours the vertices want (every 16 m) and the
					// woods' floor, and each wanted colour's nearest in a 32 x 32 x 32 table
					std::vector<std::array<float, 3>> wanted;
					for (float y = by0; y < by1; y += 16)
						for (float x = bx0; x < bx1; x += 16)
							if (std::array<float, 3> c; groundColour(x, y, c))
								wanted.push_back(c);
					for (int k = 0; k < 64; ++k)
						wanted.push_back(kForestFloor), wanted.push_back(kTownShade);
					photoPalette = Palette(wanted, photoNearest);
					photoUsual = photoNearest[(static_cast<int>(kForestFloor[0]) >> 3) << 10 | (static_cast<int>(kForestFloor[1]) >> 3) << 5 |
					                          (static_cast<int>(kForestFloor[2]) >> 3)];
					photoTown = photoNearest[(static_cast<int>(kTownShade[0]) >> 3) << 10 | (static_cast<int>(kTownShade[1]) >> 3) << 5 |
					                         (static_cast<int>(kTownShade[2]) >> 3)];
					texture::Image pal(256, 4);
					for (int row = 0; row < 4; ++row)
						for (int k = 0; k < 256; ++k)
							pal.At(k, row) = {photoPalette[k][0], photoPalette[k][1], photoPalette[k][2], 255};
					shared.push_back(texture::Make(o.name + "_pal", pal, texture::Format::Bgra));
					std::printf("photo ground: %zu ground colours sampled for the tint palette\n", wanted.size());
				}
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
			// (the pitched roofs' coverings: after the styles - their keys from coveringBase; the aerial roofs' key after them)
			coveringBase = static_cast<uint32_t>(wallMaterials.size());
			for (const Covering& cv : kRoofCoverings)
			{
				ydr::Material m = wallTemplate;
				m.Texture(kDiffuseTex, coveringTextures[&cv - kRoofCoverings]);
				m.Texture(kBumpTex, o.name + "_flat_n");
				m.Texture(kSpecularTex, &cv == &kRoofCoverings[3] ? o.name + "_asphalt_s" : o.name + "_patch_s");
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
		for (const roads::Mesh* m : {&surfaces.worn, &surfaces.sidewalk, &surfaces.kerb, &surfaces.patches, &surfaces.potholes, &surfaces.breakups, &surfaces.markings,
		         &surfaces.structure, &surfaces.railTrack, &surfaces.railBed, &surfaces.railSteel, &wires})
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
					const float cx0 = x0 + i * o.cell, cy0 = y0 + j * o.cell;
					if (photographed)
					{
						// (as the aerial pictures show the cell - its commonest class at 2 m - with the plants the game grows
						// on it: under the trees the forest's undergrowth, on dry grass a brown meadow, on lawns short grass)
						int count[7]{};
						for (float y = cy0 + 1; y < cy0 + o.cell; y += 2)
							for (float x = cx0 + 1; x < cx0 + o.cell; x += 2)
								if (const auto c = photoClass.Cell({x, y}); photoClass.In(c[0], c[1]) && photoClass.At(c[0], c[1]) < 7)
									++count[photoClass.At(c[0], c[1])];
						const int top = static_cast<int>(std::max_element(count + 1, count + 7) - count);
						f.material.push_back(top == 2 ? kLeaves | kForestOvergrown : top == 3 ? kGrassShort | kLushShort : top == 4 ? kGrass | kMeadowBrown
						                     : top == 5 || top == 1 ? kSand : top == 6 ? kConcrete : kGrass);
						// (and the colour the pictures see the ground there, the plants' tint: under the trees a leaf litter's)
						std::array<uint8_t, 3> colour{88, 74, 56};
						float sum[3]{}, seen = 0;
						for (float y = cy0 + 2; y < cy0 + o.cell; y += 4)
							for (float x = cx0 + 2; x < cx0 + o.cell; x += 4)
								if (const auto c = photoCover.Cell({x, y}); photoCover.In(c[0], c[1]))
									if (const auto& p = photoCover.At(c[0], c[1]); p[0] >= 3 && (p[1] || p[2] || p[3]))
										sum[0] += p[1], sum[1] += p[2], sum[2] += p[3], seen += 1;
						if (seen > 0)
							for (int k = 0; k < 3; ++k)
								colour[k] = static_cast<uint8_t>(std::clamp(sum[k] / seen, 0.0f, 255.0f));
						// (as the game's material bounds have it: about 0.45 of the pictures' - a dry field (165, 151, 121) there,
						// (71, 60, 46) in the game's countryside)
						for (uint8_t& k : colour)
							k = static_cast<uint8_t>(k * 0.45f);
						f.colour.push_back(colour);
						continue;
					}
					const uint8_t what = coverAt(cx0 + 0.5f * o.cell, cy0 + 0.5f * o.cell);
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
			std::string terrainDictionary = facadeDictionary;
			if (photographed)
			{
				// the ground as the aerial pictures show it: each vertex the palette's colour of the ground round it (the woods'
				// floor where they see none); the four layers' weights a picture of the tile, every 1.6 m from the pictures'
				// cover in 3 m round it (green grass, dry grass - in town paving -, sand or bare earth, leaf litter under the trees
				// and in the water), its first and last texels' centres on the tile's edges (sampled between them the texture
				// wraps: a line along every tile's edge)
				f.tint.resize(f.height.size());
				for (int j = 0; j <= per; ++j)
					for (int i = 0; i <= per; ++i)
					{
						std::array<float, 3> c;
						const float x = x0 + i * o.cell, y = y0 + j * o.cell;
						const uint8_t lc = landCoverAt ? landCoverAt(x, y) : 0;
						f.tint[f.V(i, j)] = groundColour(x, y, c)
						                        ? photoNearest[(static_cast<int>(c[0]) >> 3) << 10 | (static_cast<int>(c[1]) >> 3) << 5 | (static_cast<int>(c[2]) >> 3)]
						                    : lc >= 21 && lc <= 24 ? photoTown
						                                           : photoUsual;
					}
				// (the kinds of ground: 0 grass, 1 dry grass, 2 bare earth or sand, 3 paving, 4 leaf litter - under the woods'
				// trees and in the water, under a town's trees its lawns (NLCD's developed land): their leaf litter had shown as
				// bare brown yards wherever the game's trees cover less than the pictures' canopy; each pictures' class's)
				const auto kindAt = [&](float x, float y) {
					const auto c = photoClass.Cell({x, y});
					switch (photoClass.In(c[0], c[1]) ? photoClass.At(c[0], c[1]) : 0)
					{
					case 2:
						if (landCoverAt)
							if (const uint8_t lc = landCoverAt(x, y); lc >= 21 && lc <= 24)
								return 0;
						return 4;
					case 1: return 4;
					case 4: return 1;
					case 5: return 2;
					case 6: return 3;
					default: return 0;
					}
				};
				// the tile's four: the commonest kinds (every 2 m), in the slots where the kinds that meet most meet along the
				// weight map's edges (slots 0 and 3, 1 and 2 meet only through the other two)
				std::array<double, 5> count{};
				std::array<std::array<double, 5>, 5> meet{};
				for (float y = y0 + 1; y < y1; y += 2)
					for (float x = x0 + 1; x < x1; x += 2)
					{
						const int k = kindAt(x, y), right = kindAt(x + 2, y), up = kindAt(x, y + 2);
						count[k] += 1;
						meet[k][right] += 1, meet[right][k] += 1, meet[k][up] += 1, meet[up][k] += 1;
					}
				std::array<int, 5> order{0, 1, 2, 3, 4};
				std::stable_sort(order.begin(), order.end(), [&](int a, int b) { return count[a] > count[b]; });
				std::array<int, 4> chosen{order[0], order[1], order[2], order[3]};
				std::array<int, 4> slot = chosen;
				double best = 1e30;
				std::sort(chosen.begin(), chosen.end());
				do
				{
					const double diagonal = meet[chosen[0]][chosen[3]] + meet[chosen[1]][chosen[2]];
					if (diagonal < best)
						best = diagonal, slot = chosen;
				} while (std::next_permutation(chosen.begin(), chosen.end()));
				// (a kind left out drawn as the nearest one the tile has: paving as bare earth and back, dry grass as grass...)
				constexpr int kInstead[5][4] = {{1, 2, 4, 3}, {0, 2, 4, 3}, {3, 1, 0, 4}, {2, 1, 0, 4}, {1, 0, 2, 3}};
				std::array<int, 5> slotOf{};
				for (int k = 0; k < 5; ++k)
				{
					slotOf[k] = -1;
					for (int i = 0; i < 4; ++i)
						if (slot[i] == k)
							slotOf[k] = i;
					for (int alt = 0; slotOf[k] < 0 && alt < 4; ++alt)
						for (int i = 0; i < 4; ++i)
							if (slot[i] == kInstead[k][alt])
								slotOf[k] = i;
				}
				constexpr int kLook = 128;
				texture::Image look(kLook, kLook);
				const float texel = o.tile / (kLook - 1);
				for (int v = 0; v < kLook; ++v)
					for (int u = 0; u < kLook; ++u)
					{
						// (the shares of the four slots in the 3 x 3 m round the texel; the weight map's green and blue their
						// marginals: g the second pair's share, b the second layers' - exact for a mix of two on an edge)
						const float x = x0 + u * texel, y = y1 - v * texel;
						float l[4]{};
						for (int dy = -1; dy <= 1; ++dy)
							for (int dx = -1; dx <= 1; ++dx)
								l[slotOf[kindAt(x + dx, y + dy)]] += 1.0f / 9;
						const float g = l[2] + l[3], b = l[1] + l[3];
						look.At(u, v) = {0, static_cast<uint8_t>(std::lround(g * 255)), static_cast<uint8_t>(std::lround(b * 255)), 255};
					}
				terrainDictionary = std::format("{}_tl_{}_{}", o.name, ti, tj);
				const std::string lookName = std::format("{}_lk_{}_{}", o.name, ti, tj);
				Bytes ytd;
				if (!loader::convert::WriteTextureDictionary({texture::Make(lookName, look, texture::Format::Bc1)}, ytd, error))
					return error = terrainDictionary + ": " + error, false;
				WriteFile(o.out / (terrainDictionary + ".ytd"), ytd);
				r.parents.push_back({terrainDictionary, facadeDictionary});
				ydr::Material m = photoGround;
				for (int i = 0; i < 4; ++i)
				{
					m.Texture(loader::convert::Joaat(std::format("DiffuseTexture_layer{}", i)), photoKinds[slot[i]].first);
					m.Texture(loader::convert::Joaat(std::format("BumpTexture_layer{}", i)), photoKinds[slot[i]].second);
				}
				m.Texture(loader::convert::Joaat("lookupTexture"), lookName);
				td.materials = {m};
				ydr::Mesh mesh = terrain::TileMesh(f, 0, 0, per, cx, cy, 3.0f);
				// (coarser where flat and alike: a vertex's tint the same colour within 10 a channel - Florida's flat land, a
				// third of the triangles)
				terrain::Simplify(mesh, f, 0, 0, per, 0.04f, [&](uint32_t a, uint32_t b) {
					const auto& ca = photoPalette[mesh.vertices[a].colour0[2]];
					const auto& cb = photoPalette[mesh.vertices[b].colour0[2]];
					return std::abs(ca[0] - cb[0]) <= 10 && std::abs(ca[1] - cb[1]) <= 10 && std::abs(ca[2] - cb[2]) <= 10;
				});
				r.terrainTriangles += mesh.indices.size() / 3;
				td.lods[0].push_back(std::move(mesh));
			}
			else
				td.materials = {sandy ? ground3 : groundForest, lawnMaterial, forestMaterial};
			if (!photographed)
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
			ta.textureDictionary = loader::convert::Joaat(terrainDictionary);
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
				const uint32_t aerialKey = coveringBase + static_cast<uint32_t>(std::size(kRoofCoverings)), gravelKey = static_cast<uint32_t>(gravelStyle);
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
					    facadeSet.styles[gravelStyle], cx, cy, x0, y1, o.tile, static_cast<uint32_t>(k), meshes, cv, ct,
					    shapeRoof[k] >= 0 ? static_cast<int>(coveringBase) + shapeRoof[k] : -1, shapeRoof[k] >= 0 ? kRoofCoverings[shapeRoof[k]].metres : 3.0f);
					if (shapePorch[k].has)
					{
						const int whiteStyle = facades::Find(facadeSet, facades::Kind::House);
						AddPorch(s, shapePorch[k], static_cast<uint32_t>(whiteStyle), facadeSet.styles[whiteStyle], gravelKey, facadeSet.styles[gravelStyle],
						    shapeRoof[k] >= 0 ? static_cast<int>(coveringBase) + shapeRoof[k] : -1, shapeRoof[k] >= 0 ? kRoofCoverings[shapeRoof[k]].metres : 3.0f,
						    cx, cy, meshes, cv, ct);
					}
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
							    gravelKey, facadeSet.styles[gravelStyle], cx, cy, x0, y1, o.tile, static_cast<uint32_t>(k), lodMeshes, unusedV, unusedT,
							    shapeRoof[k] >= 0 ? static_cast<int>(coveringBase) + shapeRoof[k] : -1, shapeRoof[k] >= 0 ? kRoofCoverings[shapeRoof[k]].metres : 3.0f);
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
				pe.scaleXY = prop.scale;
				pe.scaleZ = prop.scaleZ > 0 ? prop.scaleZ : prop.scale;
				pe.radius = 6 * std::max(pe.scaleXY, pe.scaleZ);
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
			// (and its material bound, ma@: where the game grows its procedural plants - the undergrowth, the meadows' and
			// fields' grasses - tinted with the ground's colour)
			if (!f.colour.empty())
			{
				const auto plants = terrain::TileCollision(f, 0, 0, per, true);
				Bytes material;
				if (!plants.empty())
				{
					if (!ybn::Write(plants, material, error, true))
						return error = "ma@" + cname + ": " + error, false;
					WriteFile(o.out / ("ma@" + cname + ".ybn"), material);
				}
			}
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
			add(pavedMaterials[static_cast<size_t>(paving::Paving::Concrete)], surfaces.structure); // (bridges' decks, parapets, piers)
			add(railTrackMaterial, surfaces.railTrack);
			add(railBedMaterial, surfaces.railBed);
			add(railSteelMaterial, surfaces.railSteel);
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
			// the power lines' wires: a model of their own (in the roads' model, with the cable shader among its materials, the
			// asphalt, sidewalks and kerbs went undrawn - only the markings after it showed)
			{
				const auto near = meshTiles.at(&wires).Near(ti, tj);
				const roads::Mesh part = Clip(wires, x0, y0, x1, y1, cx, cy, &near);
				if (!part.indices.empty())
				{
					const std::string pname = std::format("{}_pl_{}_{}", o.name, ti, tj);
					ydr::Drawable pd;
					pd.name = pname;
					pd.materials.push_back(cableMaterial);
					AddMeshes(pd, 0, part);
					if (!ydr::Write(pd, file, error))
						return error = pname + ": " + error, false;
					WriteFile(o.out / (pname + ".ydr"), file);
					mapfiles::Archetype pa;
					pa.name = pname;
					pa.textureDictionary = loader::convert::Joaat(facadeDictionary);
					pa.lodDist = 500;
					ydr::Box(pd, pa.min, pa.max);
					r.archetypes.push_back(pa);
					mapfiles::Entity pe;
					pe.archetype = loader::convert::Joaat(pname);
					pe.position = {cx, cy, 0};
					pe.lodDist = 500;
					pe.radius = o.tile * 0.75f;
					r.entities.push_back(pe);
				}
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
		// (`cars`: car generators, with the first file's entities)
		const auto writeMaps = [&](const std::string& base, const std::vector<mapfiles::Entity>& entities, const mapfiles::MapInfo& info,
		                           const std::vector<mapfiles::CarGen>& cars = {}) {
			for (size_t first = 0, k = 0; first < entities.size(); first += kPerFile, ++k)
			{
				const std::vector<mapfiles::Entity> part(entities.begin() + static_cast<ptrdiff_t>(first),
				    entities.begin() + static_cast<ptrdiff_t>(std::min(entities.size(), first + kPerFile)));
				Bytes map;
				const std::string mname = k ? std::format("{}_{}", base, k) : base;
				if (!mapfiles::WriteMap(files, mname, part, map, error, info, k ? std::vector<mapfiles::CarGen>{} : cars))
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
		// (the parked cars: with the near maps' entities of their 250 m cell - a map of car generators alone, without
		// entities, had none of them made; the game's own are in its maps with entities too; at most a block's worth a cell,
		// 204: mapfiles::WriteMap)
		std::map<std::pair<int, int>, std::vector<mapfiles::CarGen>> carCells;
		size_t carsOver = 0;
		for (const auto& c : parked)
			if (auto& list = carCells[{static_cast<int>(std::floor((c.position[0] - w.min[0]) / 250)), static_cast<int>(std::floor((c.position[1] - w.min[1]) / 250))}];
			    list.size() < 204)
				list.push_back(c);
			else
				++carsOver;
		if (carsOver)
			std::printf("parked cars: %zu over a cell's 204 left out\n", carsOver);
		size_t carsWithout = 0;
		for (const auto& [cell, entities] : nearCells)
		{
			const auto cars = carCells.find(cell);
			if (!writeMaps(std::format("{}_n_{}_{}", o.name, cell.first, cell.second), entities, {},
			        cars != carCells.end() ? cars->second : std::vector<mapfiles::CarGen>{}))
				return false;
		}
		for (const auto& [cell, cars] : carCells)
			carsWithout += nearCells.contains(cell) ? 0 : cars.size();
		if (carsWithout)
			std::printf("parked cars: %zu in cells without trees or props left out\n", carsWithout);
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
