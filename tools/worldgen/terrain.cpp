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

		// A collision part's grid of w x h cells (vertices (w + 1) x (h + 1), two triangles a cell in order) made coarser where
		// the ground is flat: each block of 16 x 16, 8 x 8, 4 x 4 or 2 x 2 cells (the largest first, at multiples of its size)
		// of one material whose vertices all lie within 4 cm of the two triangles over its corners becomes those two
		// (Florida's flat land: a sixth of the triangles or fewer - 2 x 2 only had left three quarters, the collision most of a
		// sector's pack). Where a coarse cell meets finer ones the surfaces differ only up and down - no gap a thing could fall
		// through -, by at most those 4 cm.
		void Coarsen(ybn::Part& part, int w, int h)
		{
			const auto V = [&](int i, int j) { return static_cast<uint32_t>(j * (w + 1) + i); };
			const auto Z = [&](int i, int j) { return part.vertices[V(i, j)][2]; };
			std::vector<std::array<uint32_t, 3>> triangles;
			std::vector<uint8_t> materials;
			std::vector<uint8_t> merged(static_cast<size_t>(w) * h, 0);
			for (const int size : {16, 8, 4, 2})
				for (int bj = 0; bj + size <= h; bj += size)
					for (int bi = 0; bi + size <= w; bi += size)
					{
						const uint8_t m = part.triangleMaterials[(static_cast<size_t>(bj) * w + bi) * 2];
						bool free = true;
						for (int dj = 0; dj < size && free; ++dj)
							for (int di = 0; di < size && free; ++di)
							{
								const size_t cell = static_cast<size_t>(bj + dj) * w + bi + di;
								free = !merged[cell] && part.triangleMaterials[cell * 2] == m;
							}
						if (!free)
							continue;
						// (the coarse cell's diagonal along the flatter way; the height there of each of its vertices)
						const float za = Z(bi, bj), zb = Z(bi + size, bj), zc = Z(bi, bj + size), zd = Z(bi + size, bj + size);
						const bool ad = std::fabs(za - zd) <= std::fabs(zb - zc);
						const auto plane = [&](float u, float v) {
							if (ad) // (triangles a b d and a d c)
								return u >= v ? za + (zb - za) * u + (zd - zb) * v : za + (zd - zc) * u + (zc - za) * v;
							// (triangles a b c and b d c)
							return u + v <= 1 ? za + (zb - za) * u + (zc - za) * v : zd + (zc - zd) * (1 - u) + (zb - zd) * (1 - v);
						};
						bool flat = true;
						for (int dj = 0; dj <= size && flat; ++dj)
							for (int di = 0; di <= size && flat; ++di)
								flat = std::fabs(Z(bi + di, bj + dj) - plane(static_cast<float>(di) / size, static_cast<float>(dj) / size)) < 0.04f;
						if (!flat)
							continue;
						for (int dj = 0; dj < size; ++dj)
							for (int di = 0; di < size; ++di)
								merged[static_cast<size_t>(bj + dj) * w + bi + di] = 1;
						const uint32_t a = V(bi, bj), b = V(bi + size, bj), c = V(bi, bj + size), d = V(bi + size, bj + size);
						if (ad)
							triangles.push_back({a, b, d}), triangles.push_back({a, d, c});
						else
							triangles.push_back({a, b, c}), triangles.push_back({b, d, c});
						materials.push_back(m), materials.push_back(m);
					}
			for (int j = 0; j < h; ++j)
				for (int i = 0; i < w; ++i)
					if (!merged[static_cast<size_t>(j) * w + i])
						for (int k = 0; k < 2; ++k)
						{
							const size_t t = (static_cast<size_t>(j) * w + i) * 2 + k;
							triangles.push_back(part.triangles[t]);
							materials.push_back(part.triangleMaterials[t]);
						}
			// (the vertices no triangle uses left out)
			std::vector<int32_t> remap(part.vertices.size(), -1);
			std::vector<std::array<float, 3>> vertices;
			for (auto& t : triangles)
				for (uint32_t& i : t)
				{
					if (remap[i] < 0)
						remap[i] = static_cast<int32_t>(vertices.size()), vertices.push_back(part.vertices[i]);
					i = static_cast<uint32_t>(remap[i]);
				}
			part.vertices = std::move(vertices);
			part.triangles = std::move(triangles);
			part.triangleMaterials = std::move(materials);
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

	void Simplify(ydr::Mesh& m, const Field& f, int i0, int j0, int cells, float tolerance, const std::function<bool(uint32_t, uint32_t)>& alike)
	{
		const int n = std::min({cells, f.nx - i0, f.ny - j0});
		const int nb = n / 2; // (2 x 2 blocks a side; an odd last row and column stay as they are)
		const auto V = [&](int i, int j) { return static_cast<uint32_t>(j * (n + 1) + i); };
		const auto H = [&](int i, int j) { return f.H(i0 + i, j0 + j); };
		std::vector<uint8_t> merged(static_cast<size_t>(nb) * nb, 0);
		std::vector<uint8_t> diagonal(merged.size(), 0); // (1: the coarse cell split from its south-west to north-east corner)
		for (int bj = 0; bj < nb; ++bj)
			for (int bi = 0; bi < nb; ++bi)
			{
				const int i = 2 * bi, j = 2 * bj;
				const float za = H(i, j), zb = H(i + 2, j), zc = H(i, j + 2), zd = H(i + 2, j + 2);
				const bool ad = std::fabs(za - zd) <= std::fabs(zb - zc);
				bool ok = true;
				for (int dj = 0; dj <= 2 && ok; ++dj)
					for (int di = 0; di <= 2 && ok; ++di)
					{
						const float u = di / 2.0f, v = dj / 2.0f;
						const float plane = ad ? (u >= v ? za + (zb - za) * u + (zd - zb) * v : za + (zd - zc) * u + (zc - za) * v)
						                       : (u + v <= 1 ? za + (zb - za) * u + (zc - za) * v : zd + (zc - zd) * (1 - u) + (zb - zd) * (1 - v));
						ok = std::fabs(H(i + di, j + dj) - plane) < tolerance && alike(V(i, j), V(i + di, j + dj));
					}
				merged[static_cast<size_t>(bj) * nb + bi] = ok;
				diagonal[static_cast<size_t>(bj) * nb + bi] = ad;
			}
		const auto isMerged = [&](int bi, int bj) { return bi >= 0 && bj >= 0 && bi < nb && bj < nb && merged[static_cast<size_t>(bj) * nb + bi]; };
		std::vector<uint32_t> indices;
		const auto cell = [&](int i, int j) {
			const uint32_t a = V(i, j), b = V(i + 1, j), c = V(i, j + 1), d = V(i + 1, j + 1);
			const float ad = std::fabs(H(i, j) - H(i + 1, j + 1)), bc = std::fabs(H(i + 1, j) - H(i, j + 1));
			if (ad <= bc)
				indices.insert(indices.end(), {a, b, d, a, d, c});
			else
				indices.insert(indices.end(), {a, b, c, b, d, c});
		};
		for (int j = 0; j < n; ++j)
			for (int i = 0; i < n; ++i)
			{
				const int bi = i / 2, bj = j / 2;
				if (!isMerged(bi, bj) || i >= 2 * nb || j >= 2 * nb)
				{
					cell(i, j);
					continue;
				}
				if (i % 2 || j % 2)
					continue; // (the block: once, from its south-west cell)
				// the block's corners and the middles of its edges kept (a neighbour not merged, or the tile's edge)
				const bool keepS = bj == 0 || !isMerged(bi, bj - 1), keepN = 2 * bj + 2 >= n || !isMerged(bi, bj + 1);
				const bool keepW = bi == 0 || !isMerged(bi - 1, bj), keepE = 2 * bi + 2 >= n || !isMerged(bi + 1, bj);
				const uint32_t a = V(i, j), b = V(i + 2, j), c = V(i, j + 2), d = V(i + 2, j + 2);
				if (!keepS && !keepN && !keepW && !keepE)
				{
					if (diagonal[static_cast<size_t>(bj) * nb + bi])
						indices.insert(indices.end(), {a, b, d, a, d, c});
					else
						indices.insert(indices.end(), {a, b, c, b, d, c});
					continue;
				}
				// (a fan from the middle round the corners and the middles kept, anticlockwise)
				std::vector<uint32_t> ring{a};
				if (keepS)
					ring.push_back(V(i + 1, j));
				ring.push_back(b);
				if (keepE)
					ring.push_back(V(i + 2, j + 1));
				ring.push_back(d);
				if (keepN)
					ring.push_back(V(i + 1, j + 2));
				ring.push_back(c);
				if (keepW)
					ring.push_back(V(i, j + 1));
				const uint32_t o = V(i + 1, j + 1);
				for (size_t k = 0; k < ring.size(); ++k)
					indices.insert(indices.end(), {o, ring[k], ring[(k + 1) % ring.size()]});
			}
		// (the vertices used, in order)
		std::vector<int32_t> remap(m.vertices.size(), -1);
		std::vector<ydr::Vertex> vertices;
		for (uint32_t& i : indices)
		{
			if (remap[i] < 0)
				remap[i] = static_cast<int32_t>(vertices.size()), vertices.push_back(m.vertices[i]);
			i = static_cast<uint32_t>(remap[i]);
		}
		m.vertices = std::move(vertices);
		m.indices = std::move(indices);
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
		// (procedural: cells of 2 x 2 of the field's, 3 cm down - its plants then sink a little rather than float where the
		// ground bends between: half the triangles)
		const int step = procedural ? 2 : 1;
		const float down = procedural ? 0.03f : 0.0f;
		const auto at = [&](int k) { return std::min(k, n); };
		for (int pj = 0; pj < n; pj += 20 * step)
			for (int pi = 0; pi < n; pi += 20 * step)
			{
				ybn::Part part;
				// (materials by type - procedural: by tag and colour, the colours a part's own, 16 levels a channel)
				std::map<uint32_t, int> materialOf;
				std::map<uint32_t, uint8_t> colourOf;
				bool tagged = false;
				const int w = std::min(20, (n - pi + step - 1) / step), h = std::min(20, (n - pj + step - 1) / step);
				for (int j = 0; j <= h; ++j)
					for (int i = 0; i <= w; ++i)
					{
						const int gi = i0 + at(pi + i * step), gj = j0 + at(pj + j * step);
						part.vertices.push_back({f.x0 + gi * f.cell, f.y0 + gj * f.cell, f.H(gi, gj) - down});
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
						// (the cell's corners, and the field's cell its material is taken from: its middle's)
						const int gi = i0 + at(pi + i * step), gj = j0 + at(pj + j * step), gi1 = i0 + at(pi + (i + 1) * step), gj1 = j0 + at(pj + (j + 1) * step);
						const size_t cell = static_cast<size_t>(std::min((gj + gj1) / 2, f.ny - 1)) * f.nx + std::min((gi + gi1) / 2, f.nx - 1);
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
						const float ad = std::fabs(f.H(gi, gj) - f.H(gi1, gj1)), bc = std::fabs(f.H(gi1, gj) - f.H(gi, gj1));
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
				if (!procedural)
					Coarsen(part, w, h);
				if (!procedural || tagged)
					parts.push_back(std::move(part));
			}
		return parts;
	}
}
