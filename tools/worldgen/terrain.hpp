#pragma once
#include <array>
#include <cstdint>
#include <vector>

#include "ybn.hpp"
#include "ydr.hpp"

// Ground from a height field: a grid of heights (cells of `cell` metres from (x0, y0)), each vertex's terrain layer
// weights (the second vertex colour of the game's terrain_cb_* shaders: green picks the layer pair, blue the layer in
// it, research/phase0.md §61.12) and each cell's collision material. Cut into square tiles: a model's mesh (vertices
// relative to the tile's centre) and its collision parts (world space).
namespace worldgen::terrain
{
	struct Field
	{
		float x0 = 0, y0 = 0, cell = 2;
		int nx = 0, ny = 0;                       // cells; vertices are (nx + 1) x (ny + 1)
		std::vector<float> height;                // per vertex, row by row from the south
		std::vector<std::array<uint8_t, 4>> layers; // per vertex: the second colour (r darkens, g pair, b layer, a 255)
		std::vector<uint8_t> material;            // per cell: a row of materials.dat

		float H(int i, int j) const { return height[static_cast<size_t>(j) * (nx + 1) + i]; }
		size_t V(int i, int j) const { return static_cast<size_t>(j) * (nx + 1) + i; }
	};

	// The layer weights of a mix of the four layers (each 0..1; they are normalised).
	std::array<uint8_t, 4> Layers(float l0, float l1, float l2, float l3);

	// The mesh of the tile of `cells` x `cells` from cell (i0, j0), around (cx, cy); texture coordinates: world
	// metres / uvMetres.
	ydr::Mesh TileMesh(const Field& f, int i0, int j0, int cells, float cx, float cy, float uvMetres);
	// Its collision: parts of at most 20 x 20 cells.
	std::vector<ybn::Part> TileCollision(const Field& f, int i0, int j0, int cells);
}
