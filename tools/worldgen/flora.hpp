#pragma once
#include <array>
#include <cstdint>
#include <vector>

#include "roads.hpp"
#include "texture.hpp"

// Trees of our own (research/phase0.md §63.2): north Florida's - the game's are California's pines and England's oaks, and
// every street picture of Tallahassee is framed by live oaks hung with Spanish moss and loblolly pines bare to their high
// crowns. Each tree a model made here from a number: its trunk and limbs (tubes of bark), its crown's leaf clusters (cards
// cut out of a picture drawn here, both faces, their normals pointing out from the crown's middle as the game's own trees'
// do, so a crown is lit as a whole), a live oak's moss hanging from its limbs, a crape myrtle's flowers; four levels of
// detail (cards fewer and bigger, tubes thinner further off).
namespace worldgen::flora
{
	enum class Species : uint8_t
	{
		LiveOak, // short trunk, a few huge limbs out almost level, a low wide crown, Spanish moss
		Pine,    // loblolly: a straight trunk bare to the crown in its top third, whorls of up-swept branches
		Oak,     // laurel and water oaks: a taller trunk, climbing limbs, a round crown
		Myrtle,  // crape myrtle: a few thin smooth stems from the ground, a vase of leaves and flowers
		Shrub,   // a yard's or a wood's shrub: a dense dark green mound (ligustrum, holly, wax myrtle), no trunk to see
		Magnolia, // southern magnolia: a straight trunk, a dense dark egg of a crown pointed at the top, big glossy leaves
		Sweetgum, // a central leader up the whole tree, loose whorls of level branches - a pyramid, an oval later; star leaves
	};
	constexpr int kSpecies = 7;

	// The pictures (drawn here; leaves and flowers four clusters in a 2 x 2 atlas, alpha their outline): what each part of
	// a model is drawn with.
	enum Texture : uint8_t
	{
		kOakLeaves,
		kPineNeedles,
		kMoss,
		kMyrtle,
		kMagnoliaLeaves,
		kGumLeaves,
		kBarkOak,
		kBarkPine,
		kBarkMyrtle,
		kBarkSmooth, // (a magnolia's: smooth grey, lenticels)
		kTextures,
	};
	texture::Image Picture(Texture t);
	// a picture with an outline (alpha) or a bark (opaque)
	constexpr bool Cutout(Texture t) { return t <= kGumLeaves; }

	struct Model
	{
		// per level of detail (0 the nearest), per picture: its triangles, positions from the trunk's foot
		std::array<std::array<roads::Mesh, kTextures>, 4> lods;
		float height = 0, width = 0; // its size as made (a placement scales it to the tree's)
		float trunk = 0;             // the trunk's radius at its foot (its collision)
	};
	Model Make(Species s, uint32_t seed);
}
