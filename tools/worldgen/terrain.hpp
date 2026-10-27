#pragma once
#include <array>
#include <functional>
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
		std::vector<uint16_t> material;           // per cell: a row of materials.dat; its high byte a procedural tag
		                                          // (procedural.meta's procTagTable: plants the game grows round the player)
		std::vector<std::array<uint8_t, 3>> colour; // per cell (empty: none): the ground's colour, the plants' tint
		// lawns (TileMeshes; empty: none): per cell, drawn with the lawn material; per vertex, how far into the lawn (0
		// where it meets the other material: there both draw the same grass, the ground material's layer 0 and the lawn's
		// layer 3; 1 inside, where the lawn's blend map mixes its four grasses)
		std::vector<uint8_t> lawnCell;
		std::vector<float> lawn;
		// forest floors (as the lawns, with a material of leaf litters, grass and earth: its layer 3 the ground material's
		// leaf litter): per cell, per vertex how far in
		std::vector<uint8_t> forestCell;
		std::vector<float> forest;
		// the ground as the aerial pictures show it (research/phase0.md §63; empty: not): per vertex the tint palette's
		// column (the first colour's blue), and the second texture coordinates across the tile, 0..1 from its north-west
		// corner (its picture of the four layers' weights)
		std::vector<uint8_t> tint;

		float H(int i, int j) const { return height[static_cast<size_t>(j) * (nx + 1) + i]; }
		size_t V(int i, int j) const { return static_cast<size_t>(j) * (nx + 1) + i; }
	};

	// The layer weights of a mix of the four layers (each 0..1; they are normalised).
	std::array<uint8_t, 4> Layers(float l0, float l1, float l2, float l3);

	// The mesh of the tile of `cells` x `cells` from cell (i0, j0), around (cx, cy); texture coordinates: world
	// metres / uvMetres.
	ydr::Mesh TileMesh(const Field& f, int i0, int j0, int cells, float cx, float cy, float uvMetres);
	// TileMesh's mesh made coarser (research/phase0.md §63): each block of 16 x 16, 8 x 8, 4 x 4 or 2 x 2 cells (the
	// largest first, at multiples of its size) whose vertices all lie within `tolerance` of the two triangles over its
	// corners and are `alike` (vertex indices: their tints) its south-west corner becomes those two triangles - or a fan
	// from its middle round the vertices of its edge that the blocks and cells round it keep, and all of the tile's edge
	// (the neighbouring tile's mesh), so that no crack opens. Vertices no triangle uses are left out. `slack` (field vertex
	// indices; none: 0): more tolerance there - a block's the least of its vertices'.
	void Simplify(ydr::Mesh& m, const Field& f, int i0, int j0, int cells, float tolerance, const std::function<bool(uint32_t, uint32_t)>& alike,
	    const std::function<float(int, int)>& slack = {});
	// The same split by material (research/phase0.md §61.25): the lawn cells (Field::lawnCell) in a mesh of their own
	// for the game's lawn shader (terrain_cb_w_4lyr_2tex_blend: its blend map, on the second texture coordinates every
	// `blendMetres`, mixes four grasses where the vertex colour's alpha is 0; at alpha 1 the vertex colour picks layer 3),
	// the rest as TileMesh.
	struct Meshes
	{
		ydr::Mesh ground, lawn, forest;
	};
	Meshes TileMeshes(const Field& f, int i0, int j0, int cells, float cx, float cy, float uvMetres, float blendMetres);
	// Its collision: parts of at most 20 x 20 cells, made coarser where flat (blocks of up to 16 x 16 cells of one material
	// within 4 cm; WG_COARSE: the whole tile in blocks of up to 32 x 32 within 8 cm, the soft grounds mixed - cars froze on
	// it, see terrain.cpp). `procedural`: instead the game's material bounds (ma@ files, research/
	// phase0.md §63: the only collision its procedural plants are looked for in) - each cell its material's procedural tag
	// (Field::material's high byte) and its ground colour (Field::colour), every vertex its colour and how dense the plants
	// grow; the parts without a tagged cell left out.
	// `slack` as Simplify's.
	std::vector<ybn::Part> TileCollision(const Field& f, int i0, int j0, int cells, bool procedural = false, const std::function<float(int, int)>& slack = {});
}
