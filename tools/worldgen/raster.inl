#pragma once
#include <algorithm>
#include <cmath>

namespace worldgen::raster
{
	template <typename T, typename F>
	void ForPolygon(const Grid<T>& g, const std::vector<std::vector<P2>>& rings, F&& visit)
	{
		float minY = 1e30f, maxY = -1e30f;
		for (const auto& r : rings)
			for (const P2& p : r)
				minY = std::min(minY, p[1]), maxY = std::max(maxY, p[1]);
		const int j0 = std::max(0, static_cast<int>(std::floor((minY - g.y0) / g.cell))), j1 = std::min(g.ny - 1, static_cast<int>(std::ceil((maxY - g.y0) / g.cell)));
		std::vector<float> xs;
		for (int j = j0; j <= j1; ++j)
		{
			const float y = g.y0 + (j + 0.5f) * g.cell;
			xs.clear();
			for (const auto& r : rings)
				for (size_t k = 0; k < r.size(); ++k)
				{
					const P2& a = r[k];
					const P2& b = r[(k + 1) % r.size()];
					if ((a[1] <= y) != (b[1] <= y))
						xs.push_back(a[0] + (y - a[1]) / (b[1] - a[1]) * (b[0] - a[0]));
				}
			std::sort(xs.begin(), xs.end());
			for (size_t k = 0; k + 1 < xs.size(); k += 2)
			{
				const int i0 = std::max(0, static_cast<int>(std::ceil((xs[k] - g.x0) / g.cell - 0.5f)));
				const int i1 = std::min(g.nx - 1, static_cast<int>(std::floor((xs[k + 1] - g.x0) / g.cell - 0.5f)));
				for (int i = i0; i <= i1; ++i)
					visit(i, j);
			}
		}
	}

	template <typename T>
	void FillPolygon(Grid<T>& g, const std::vector<std::vector<P2>>& rings, T value)
	{
		ForPolygon(g, rings, [&](int i, int j) { g.At(i, j) = value; });
	}

	template <typename T>
	void DrawLine(Grid<T>& g, const std::vector<P2>& line, float half, T value)
	{
		for (size_t k = 0; k + 1 < line.size(); ++k)
		{
			const P2& a = line[k];
			const P2& b = line[k + 1];
			const float minX = std::min(a[0], b[0]) - half, maxX = std::max(a[0], b[0]) + half, minY = std::min(a[1], b[1]) - half,
			            maxY = std::max(a[1], b[1]) + half;
			const int i0 = std::max(0, static_cast<int>((minX - g.x0) / g.cell)), i1 = std::min(g.nx - 1, static_cast<int>((maxX - g.x0) / g.cell));
			const int j0 = std::max(0, static_cast<int>((minY - g.y0) / g.cell)), j1 = std::min(g.ny - 1, static_cast<int>((maxY - g.y0) / g.cell));
			const float dx = b[0] - a[0], dy = b[1] - a[1], len2 = dx * dx + dy * dy;
			for (int j = j0; j <= j1; ++j)
				for (int i = i0; i <= i1; ++i)
				{
					const P2 c = g.Centre(i, j);
					float t = len2 > 0 ? ((c[0] - a[0]) * dx + (c[1] - a[1]) * dy) / len2 : 0;
					t = std::clamp(t, 0.0f, 1.0f);
					const float ex = a[0] + t * dx - c[0], ey = a[1] + t * dy - c[1];
					if (ex * ex + ey * ey <= half * half)
						g.At(i, j) = value;
				}
		}
	}
}
