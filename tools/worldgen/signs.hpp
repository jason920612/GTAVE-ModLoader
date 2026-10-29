#pragma once
#include <array>
#include <cstddef>
#include <functional>
#include <string>
#include <vector>

#include "props.hpp"
#include "roads.hpp"
#include "texture.hpp"

// Street name signs (research/phase0.md §70): the green blades at the junctions - two, crossed, each along the street it
// names (white mixed-case letters on green, a white border inside the edge), on top of the junction's stop sign where it
// has one, else on a galvanised post of their own at the corner. The letters are cards of one small picture of the
// font's glyphs - not a picture per name: a land has two thousand street names, and the pictures' memory is the
// graphics card's.
namespace worldgen::signs
{
	struct Meshes
	{
		roads::Mesh blades;  // the blades' green (a plain green picture: the barriers' steel shader)
		roads::Mesh letters; // the letters and the borders (FontPicture cut out - the chain link's shader; each face its own)
		roads::Mesh posts;   // galvanised posts (the barriers' steel)
	};

	// The glyphs' picture: the font's printable ASCII glyphs white on transparent, 16 to a row in cells of 64 pixels, and
	// the last cell solid white (the borders).
	texture::Image FontPicture();

	// A street's name as its sign has it: "Southwest 54th Terrace" -> "SW 54th Ter", "Shore Drive South" -> "Shore Dr S",
	// the first of names joined by '/', letters outside ASCII without their accents.
	std::string Abbreviate(const std::string& name);

	struct Stats
	{
		size_t junctions = 0, onStops = 0, posts = 0;
	};
	// The junctions of two named streets or more in `box` (x0, y0, x1, y1): the blades on the stop sign nearest the junction
	// (`furniture`'s), else on a post of their own on the right of the least of the streets coming in (`open(x, y)`: not on
	// a road or in a building; `ground(x, y)`: the terrain's height). The posts' collision into `collision`.
	void Build(const roads::Network& n, const std::vector<props::Prop>& furniture, const std::function<bool(float, float)>& open,
	    const std::function<float(float, float)>& ground, const std::array<float, 4>& box, Meshes& out, roads::Collision& collision, Stats& stats);
}
