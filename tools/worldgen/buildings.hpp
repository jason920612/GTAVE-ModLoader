#pragma once
#include <array>
#include <cstdint>
#include <string>
#include <vector>

#include "catalog.hpp"
#include "world.hpp"

// Buildings of the game placed on the world's footprints (research/phase0.md §61.17): each footprint's oriented box
// (the narrowest rectangle round it) against the catalog's city buildings' boxes - long side, short side, height; a
// model may be smaller than the footprint but hardly bigger, so it does not reach into the street - one of the closest
// few picked (by the footprint's place: the same world builds the same), turned to the footprint and set on the ground.
// Its collision: the box (the game keeps building collision in the areas' files, not with the model).
namespace worldgen::buildings
{
	using P2 = std::array<float, 2>;
	using P3 = std::array<float, 3>;

	struct Placement
	{
		const catalog::Item* item = nullptr;
		P3 position{};   // the entity's
		float heading = 0; // radians, anticlockwise from the model's x axis to east
		P2 centre{};     // the footprint's box
		float lodDist = 500;
	};

	class Matcher
	{
	public:
		explicit Matcher(const std::vector<catalog::Item>& catalog);
		size_t Candidates() const { return candidates_.size(); }
		// A model for a footprint of this height standing on `ground` (its lowest point under the footprint), or false.
		// `kind`: the OSM building type (house, apartments, commercial, industrial...): which of the game's areas'
		// buildings fit (houses from the residential hills and Vespucci; apartments from Vespucci, Del Perro,
		// Hollywood; shops and offices from the city; industrial ones only for industrial buildings).
		bool Pick(const std::vector<P2>& footprint, float height, float ground, const std::string& kind, Placement& out) const;

	private:
		enum class Style : uint8_t
		{
			House,
			Apartments,
			City,
			Industrial,
		};
		struct Candidate
		{
			const catalog::Item* item;
			float longSide, shortSide, height;
			bool longIsX;
			uint8_t styles; // a bit per Style it suits
		};
		std::vector<Candidate> candidates_;
	};

	// The oriented box of a footprint: centre, the long side's direction (radians), long and short sides.
	void OrientedBox(const std::vector<P2>& footprint, P2& centre, float& angle, float& longSide, float& shortSide);

	// The collision box of a placement: 8 corners, 12 triangles (outward faces).
	void CollisionBox(const Placement& p, std::vector<P3>& vertices, std::vector<std::array<uint32_t, 3>>& triangles);
}
