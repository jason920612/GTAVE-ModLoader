#pragma once
#include <array>
#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

// Grids over the world (cells of `cell` metres from (x0, y0), row by row from the south): drawing polygons and thick
// lines into them, which side of the coastline each cell is on, distances, and pictures (PNG) for checking.
namespace worldgen::raster
{
	using P2 = std::array<float, 2>;

	template <typename T>
	struct Grid
	{
		float x0 = 0, y0 = 0, cell = 4;
		int nx = 0, ny = 0;
		std::vector<T> v;

		void Init(float minX, float minY, float maxX, float maxY, float size, T fill)
		{
			x0 = minX, y0 = minY, cell = size;
			nx = static_cast<int>((maxX - minX) / size) + 1, ny = static_cast<int>((maxY - minY) / size) + 1;
			v.assign(static_cast<size_t>(nx) * ny, fill);
		}
		T& At(int i, int j) { return v[static_cast<size_t>(j) * nx + i]; }
		const T& At(int i, int j) const { return v[static_cast<size_t>(j) * nx + i]; }
		bool In(int i, int j) const { return i >= 0 && j >= 0 && i < nx && j < ny; }
		P2 Centre(int i, int j) const { return {x0 + (i + 0.5f) * cell, y0 + (j + 0.5f) * cell}; }
		// the cell holding a point (may be outside)
		std::array<int, 2> Cell(const P2& p) const { return {static_cast<int>(std::floor((p[0] - x0) / cell)), static_cast<int>(std::floor((p[1] - y0) / cell))}; }
	};

	// Every cell whose centre is inside the polygon (even-odd over all rings: outer rings and holes together): visit(i, j).
	template <typename T, typename F>
	void ForPolygon(const Grid<T>& g, const std::vector<std::vector<P2>>& rings, F&& visit);
	// Sets those cells.
	template <typename T>
	void FillPolygon(Grid<T>& g, const std::vector<std::vector<P2>>& rings, T value);

	// Sets every cell whose centre is within `half` metres of the polyline.
	template <typename T>
	void DrawLine(Grid<T>& g, const std::vector<P2>& line, float half, T value);

	// Land (1) or water (0) for every cell by the coastlines (land on their left): the side of the nearest coastline.
	// Cells farther than `reach` metres from every coastline keep their value.
	void ClassifyCoast(Grid<uint8_t>& land, const std::vector<std::vector<P2>>& coastlines, float reach);

	// The distance (m) from every cell to the nearest cell where `inside` is false (0 for those), approximately (a two-pass
	// chamfer).
	Grid<float> DistanceTo(const Grid<uint8_t>& inside, uint8_t value);

	// A picture: rgb per cell, north up.
	bool WritePng(const std::filesystem::path& file, int width, int height, const std::vector<std::array<uint8_t, 3>>& rgb, std::string& error);
}

#include "raster.inl"
