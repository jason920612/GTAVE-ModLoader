#pragma once
#include <array>
#include <cstdint>
#include <string>
#include <vector>

#include "convert/ytd.hpp"

// The walls' looks for buildings of our own (research/phase0.md §61.18): textures drawn here (no picture taken from
// anyone), each one tile of a wall - floors of windows in bays - and how many metres it covers, with a normal map
// (frames stand out, glass sits back) and a specular map (glass shines, plaster does not). Styles for Miami: plastered
// houses, pastel Art Deco walk-ups, concrete mid-rises with ribbon windows, glass towers, white towers with balconies,
// metal sheds, shop fronts, and the gravel of flat roofs no picture covers; and for the rest of Florida, as the street
// pictures show it (Mapillary's of Tallahassee): frame houses in lap siding with white trim and shutters, brick houses,
// red brick walk-ups and offices (downtowns, campuses).
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
		Siding,    // a frame house's lap siding
		Brick,     // a brick house
		BrickCity, // red brick floors of 3 m: walk-ups, offices, campuses
		Garage,    // a parking garage's open decks: concrete spandrels, dark decks, columns
	};

	struct Style
	{
		Kind kind = Kind::House;
		std::string diffuse, normal, specular; // texture names (a kind's styles share the last two)
		float width = 8, height = 6;           // metres one texture covers
		std::array<uint8_t, 3> average{};      // the texture's mean colour (the LOD models' walls)
	};

	struct Set
	{
		std::vector<Style> styles;
		std::vector<loader::convert::DictionaryTexture> textures;
		std::string palette; // a texture of each style's mean colour, 8 x 8 pixels each (16 a row): the LODs' walls
		// the middle of a style's patch in the palette (u, v)
		std::array<float, 2> PaletteUv(size_t style) const;
	};

	// The styles and their textures, names starting with `prefix`; `size` the diffuse textures' side (pixels) for the walls
	// of fine detail - siding, brick -, half that for the others; only the region's (`latitude`): Art Deco in Miami, siding
	// and brick elsewhere (a pack's resource is at most 16 MB: all of them did not fit one texture dictionary).
	Set Make(const std::string& prefix, int size, double latitude = 25.8);

	// What the street pictures saw of a building's walls (tools/worldgen/photofacade.py): their lit colour, the share that
	// looks like brick.
	struct Seen
	{
		std::array<uint8_t, 3> colour{};
		float brick = 0;
	};

	// A building's wall style: by its map kind, height (m), footprint area (m²), latitude (north Florida's houses are
	// mostly frame and brick, south Florida's concrete block and stucco; Art Deco only in Miami) and a number of its own;
	// where the street pictures saw it, their material - brick or painted - in the style of it nearest their colour.
	// `year`: the year it was built (the property rolls; 0 not known): north Florida's houses by their era - frame houses
	// before 1940, brick ranches 1940-1979, siding and brick after, stucco more from 2000.
	int Pick(const Set& set, const std::string& kind, float height, float area, uint32_t seed, double latitude = 25.8, const Seen* seen = nullptr,
	    int year = 0);
	int Find(const Set& set, Kind kind);
}
