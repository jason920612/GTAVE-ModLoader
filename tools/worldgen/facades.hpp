#pragma once
#include <cstdint>
#include <string>
#include <vector>

#include "convert/ytd.hpp"

// The walls' looks for buildings of our own (research/phase0.md §61.18): textures drawn here (no picture taken from
// anyone), each one tile of a wall - floors of windows in bays - and how many metres it covers, with a normal map
// (frames stand out, glass sits back) and a specular map (glass shines, plaster does not). Styles for Miami: plastered
// houses, pastel Art Deco walk-ups, concrete mid-rises with ribbon windows, glass towers, white towers with balconies,
// metal sheds, shop fronts, and the gravel of flat roofs no picture covers.
namespace worldgen::facades
{
	enum class Kind : uint8_t
	{
		House,
		Deco,
		Concrete,
		Glass,
		Balcony,
		Shed,
		Shop, // a ground floor of shop windows
		Roof, // a flat roof (mapped from above)
	};

	struct Style
	{
		Kind kind = Kind::House;
		std::string diffuse, normal, specular; // texture names (a kind's styles share the last two)
		float width = 8, height = 6;           // metres one texture covers
	};

	struct Set
	{
		std::vector<Style> styles;
		std::vector<loader::convert::DictionaryTexture> textures;
	};

	// The styles and their textures, names starting with `prefix`; `size` the diffuse textures' side (pixels).
	Set Make(const std::string& prefix, int size);

	// A building's wall style: by its map kind, height (m), footprint area (m²) and a number of its own.
	int Pick(const Set& set, const std::string& kind, float height, float area, uint32_t seed);
	int Find(const Set& set, Kind kind);
}
