#include "terrain.hpp"

#include <algorithm>
#include <cmath>
#include <map>

namespace worldgen::terrain
{
	namespace
	{
		uint8_t Byte(float v)
		{
			return static_cast<uint8_t>(std::lround(std::clamp(v, 0.0f, 1.0f) * 255.0f));
		}

		// Smooth value noise in [-1, 1] over a lattice of `cell` metres.
		float Noise(float x, float y, float cell, uint32_t seed)
		{
			const auto at = [&](int i, int j) {
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

		std::array<float, 3> Normalised(std::array<float, 3> v)
		{
			const float l = std::hypot(v[0], v[1], v[2]);
			if (l > 0)
				for (float& c : v)
					c /= l;
			return v;
		}
	}

	std::array<uint8_t, 4> Layers(float l0, float l1, float l2, float l3)
	{
		const float sum = l0 + l1 + l2 + l3;
		if (sum <= 0)
			return {0, 0, 0, 255};
		l0 /= sum, l1 /= sum, l2 /= sum, l3 /= sum;
		// the shader's weights are (1-g)(1-b), (1-g)b, g(1-b), gb: g the second pair's share, b the second layers'
		return {0, Byte(l2 + l3), Byte(l1 + l3), 255};
	}

	ydr::Mesh TileMesh(const Field& f, int i0, int j0, int cells, float cx, float cy, float uvMetres)
	{
		ydr::Mesh m;
		const int n = std::min({cells, f.nx - i0, f.ny - j0});
		for (int j = 0; j <= n; ++j)
			for (int i = 0; i <= n; ++i)
			{
				const int gi = i0 + i, gj = j0 + j;
				const float x = f.x0 + gi * f.cell, y = f.y0 + gj * f.cell;
				// the slope from the neighbours (one-sided at the field's edge)
				const int l = std::max(gi - 1, 0), r = std::min(gi + 1, f.nx), b = std::max(gj - 1, 0), t = std::min(gj + 1, f.ny);
				const float dx = (f.H(r, gj) - f.H(l, gj)) / ((r - l) * f.cell), dy = (f.H(gi, t) - f.H(gi, b)) / ((t - b) * f.cell);
				ydr::Vertex v;
				v.position = {x - cx, y - cy, f.H(gi, gj)};
				v.normal = Normalised({-dx, -dy, 1});
				const std::array<float, 3> tangent = Normalised({1, 0, dx});
				v.tangent = {tangent[0], tangent[1], tangent[2], 1};
				v.colour0 = {255, 0, 255, 255}; // (no darkening; as the game's own terrain has it)
				if (!f.layers.empty())
					v.colour1 = f.layers[f.V(gi, gj)];
				if (!f.tint.empty())
				{
					// (the weight map's first and last texels' centres on the tile's edges: 128 texels)
					v.colour0[2] = f.tint[f.V(gi, gj)];
					v.uv[1] = {(0.5f + 127.0f * i / n) / 128.0f, (0.5f + 127.0f * (n - j) / n) / 128.0f};
				}
				// (from a whole number of repeats near the tile's middle: the same texture at the tiles' seams, and small
				// coordinates - in the thousands they lose their precision on the way to the texture, which then shows
				// as a grid of blocks)
				v.uv[0] = {x / uvMetres - std::floor(cx / uvMetres), -y / uvMetres - std::floor(-cy / uvMetres)};
				m.vertices.push_back(v);
			}
		for (int j = 0; j < n; ++j)
			for (int i = 0; i < n; ++i)
			{
				const uint32_t a = j * (n + 1) + i, b = a + 1, c = a + n + 1, d = c + 1;
				// (the diagonal along the flatter way)
				const float ad = std::fabs(f.H(i0 + i, j0 + j) - f.H(i0 + i + 1, j0 + j + 1)),
				            bc = std::fabs(f.H(i0 + i + 1, j0 + j) - f.H(i0 + i, j0 + j + 1));
				if (ad <= bc)
					m.indices.insert(m.indices.end(), {a, b, d, a, d, c});
				else
					m.indices.insert(m.indices.end(), {a, b, c, b, d, c});
			}
		return m;
	}

	Meshes TileMeshes(const Field& f, int i0, int j0, int cells, float cx, float cy, float uvMetres, float blendMetres)
	{
		const ydr::Mesh all = TileMesh(f, i0, j0, cells, cx, cy, uvMetres);
		if (f.lawnCell.empty())
			return {all, {}};
		Meshes out;
		const int n = std::min({cells, f.nx - i0, f.ny - j0});
		std::vector<int> groundIndex(all.vertices.size(), -1), lawnIndex(all.vertices.size(), -1), forestIndex(all.vertices.size(), -1);
		const auto take = [&](ydr::Mesh& m, std::vector<int>& index, uint32_t v, int kind) {
			const bool lawn = kind != 0;
			if (index[v] < 0)
			{
				ydr::Vertex vertex = all.vertices[v];
				if (lawn)
				{
					// The weights: lerp(blend map, vertex colour, its alpha). Inside mostly the blend map's (the build's own,
					// noise over 128 m: no repeat a grid shows in), the colour a quarter - mostly the first pair, two even
					// grasses, varying over tens of metres; towards the edge layer 3 (green and blue full, alpha 1), the
					// ground material's grass.
					const int vi = static_cast<int>(v) % (n + 1), vj = static_cast<int>(v) / (n + 1);
					const float x = vertex.position[0] + cx, y = vertex.position[1] + cy;
					const float l = kind == 2 ? f.forest[f.V(i0 + vi, j0 + vj)] : f.lawn[f.V(i0 + vi, j0 + vj)];
					const float gIn = std::clamp(0.12f + 0.12f * Noise(x, y, 53.0f, 7), 0.0f, 1.0f), bIn = std::clamp(0.5f + 0.4f * Noise(x, y, 37.0f, 3), 0.0f, 1.0f);
					vertex.colour1 = {vertex.colour1[0], Byte(1 + (gIn - 1) * l), Byte(1 + (bIn - 1) * l), Byte(1 + (0.25f - 1) * l)}; // (red: the ground's darkening, the same where the two meet)
					vertex.uv[1] = {x / blendMetres - std::floor(cx / blendMetres), -y / blendMetres - std::floor(-cy / blendMetres)};
				}
				index[v] = static_cast<int>(m.vertices.size());
				m.vertices.push_back(vertex);
			}
			m.indices.push_back(static_cast<uint32_t>(index[v]));
		};
		// (the whole mesh's triangles come in pairs, cell by cell, as TileMesh made them)
		for (int j = 0; j < n; ++j)
			for (int i = 0; i < n; ++i)
			{
				const size_t cell = static_cast<size_t>(j0 + j) * f.nx + (i0 + i);
				const int kind = f.lawnCell[cell] ? 1 : !f.forestCell.empty() && f.forestCell[cell] ? 2 : 0;
				const size_t first = (static_cast<size_t>(j) * n + i) * 6;
				for (size_t k = first; k < first + 6; ++k)
					take(kind == 1 ? out.lawn : kind == 2 ? out.forest : out.ground, kind == 1 ? lawnIndex : kind == 2 ? forestIndex : groundIndex,
					    all.indices[k], kind);
			}
		return out;
	}

	std::vector<ybn::Part> TileCollision(const Field& f, int i0, int j0, int cells, bool procedural)
	{
		std::vector<ybn::Part> parts;
		const int n = std::min({cells, f.nx - i0, f.ny - j0});
		for (int pj = 0; pj < n; pj += 20)
			for (int pi = 0; pi < n; pi += 20)
			{
				ybn::Part part;
				// (materials by type - procedural: by tag and colour, the colours a part's own, 16 levels a channel)
				std::map<uint32_t, int> materialOf;
				std::map<uint32_t, uint8_t> colourOf;
				bool tagged = false;
				const int w = std::min(20, n - pi), h = std::min(20, n - pj);
				for (int j = 0; j <= h; ++j)
					for (int i = 0; i <= w; ++i)
					{
						const int gi = i0 + pi + i, gj = j0 + pj + j;
						part.vertices.push_back({f.x0 + gi * f.cell, f.y0 + gj * f.cell, f.H(gi, gj)});
						if (procedural)
						{
							// (the vertex's cell's colour, blue green red as the game stores it, and in the alpha how the plants
							// grow there: 56, density 7 of 7 in bits 3-5, as most of the game's countryside has it)
							const size_t cell = static_cast<size_t>(std::min(gj, f.ny - 1)) * f.nx + std::min(gi, f.nx - 1);
							const auto& c = f.colour[cell];
							part.vertexColours.push_back({c[2], c[1], c[0], static_cast<uint8_t>((f.material[cell] >> 8) ? 56 : 0)});
						}
					}
				for (int j = 0; j < h; ++j)
					for (int i = 0; i < w; ++i)
					{
						const int gi = i0 + pi + i, gj = j0 + pj + j;
						const size_t cell = static_cast<size_t>(gj) * f.nx + gi;
						const uint16_t type = f.material[cell];
						uint32_t key = type & 0xFF;
						std::array<uint8_t, 8> material = ybn::Material(static_cast<uint8_t>(type & 0xFF));
						if (procedural)
						{
							// (a material bound's: type 0, the procedural tag and the ground's colour)
							uint8_t colour = 0;
							if (const uint8_t tag = static_cast<uint8_t>(type >> 8))
							{
								tagged = true;
								const auto& c = f.colour[cell];
								const uint32_t ck = static_cast<uint32_t>(c[0] >> 4) << 8 | static_cast<uint32_t>(c[1] >> 4) << 4 | static_cast<uint32_t>(c[2] >> 4);
								auto it = colourOf.find(ck);
								if (it == colourOf.end() && part.colours.size() < 64)
								{
									part.colours.push_back({static_cast<uint8_t>((c[2] & 0xF0) | 8), static_cast<uint8_t>((c[1] & 0xF0) | 8),
									    static_cast<uint8_t>((c[0] & 0xF0) | 8), 0xCF});
									it = colourOf.emplace(ck, static_cast<uint8_t>(part.colours.size())).first;
								}
								colour = it != colourOf.end() ? it->second : 1;
							}
							key = (type >> 8) | static_cast<uint32_t>(colour) << 8;
							material = ybn::Material(0, static_cast<uint8_t>(type >> 8), colour);
						}
						if (!materialOf.contains(key))
						{
							materialOf[key] = static_cast<int>(part.materials.size());
							part.materials.push_back(material);
						}
						const uint8_t materialIndex = static_cast<uint8_t>(materialOf[key]);
						const uint32_t a = j * (w + 1) + i, b = a + 1, c = a + w + 1, d = c + 1;
						const float ad = std::fabs(f.H(gi, gj) - f.H(gi + 1, gj + 1)), bc = std::fabs(f.H(gi + 1, gj) - f.H(gi, gj + 1));
						if (ad <= bc)
						{
							part.triangles.push_back({a, b, d});
							part.triangles.push_back({a, d, c});
						}
						else
						{
							part.triangles.push_back({a, b, c});
							part.triangles.push_back({b, d, c});
						}
						part.triangleMaterials.push_back(materialIndex);
						part.triangleMaterials.push_back(materialIndex);
					}
				if (!procedural || tagged)
					parts.push_back(std::move(part));
			}
		return parts;
	}
}
