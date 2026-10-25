#include "terrain.hpp"

#include <algorithm>
#include <cmath>

namespace worldgen::terrain
{
	namespace
	{
		uint8_t Byte(float v)
		{
			return static_cast<uint8_t>(std::lround(std::clamp(v, 0.0f, 1.0f) * 255.0f));
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
				v.colour1 = f.layers[f.V(gi, gj)];
				v.uv[0] = {x / uvMetres, -y / uvMetres};
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

	std::vector<ybn::Part> TileCollision(const Field& f, int i0, int j0, int cells)
	{
		std::vector<ybn::Part> parts;
		const int n = std::min({cells, f.nx - i0, f.ny - j0});
		for (int pj = 0; pj < n; pj += 20)
			for (int pi = 0; pi < n; pi += 20)
			{
				ybn::Part part;
				std::vector<int> materialOf(256, -1);
				const int w = std::min(20, n - pi), h = std::min(20, n - pj);
				for (int j = 0; j <= h; ++j)
					for (int i = 0; i <= w; ++i)
					{
						const int gi = i0 + pi + i, gj = j0 + pj + j;
						part.vertices.push_back({f.x0 + gi * f.cell, f.y0 + gj * f.cell, f.H(gi, gj)});
					}
				for (int j = 0; j < h; ++j)
					for (int i = 0; i < w; ++i)
					{
						const int gi = i0 + pi + i, gj = j0 + pj + j;
						const uint8_t type = f.material[static_cast<size_t>(gj) * f.nx + gi];
						if (materialOf[type] < 0)
						{
							materialOf[type] = static_cast<int>(part.materials.size());
							part.materials.push_back(ybn::Material(type));
						}
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
						part.triangleMaterials.push_back(static_cast<uint8_t>(materialOf[type]));
						part.triangleMaterials.push_back(static_cast<uint8_t>(materialOf[type]));
					}
				parts.push_back(std::move(part));
			}
		return parts;
	}
}
