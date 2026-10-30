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
		RoyalPalm,   // a smooth pale grey column swollen a third of the way up, a green crownshaft, feather fronds arching
		CoconutPalm, // a ringed grey-brown trunk leaning out and curving up, a crown of drooping feather fronds
		SabalPalm,   // the cabbage palm: a trunk in criss-cross boots, a round head of fan leaves on long stalks
		ArecaPalm,   // a clump of thin ringed yellow-green stems, each with a few arching feather fronds (yards, hedges)
		Hedge,       // a clipped hedge's piece: a box of small leaves 2.6 m long (along y), 0.9 m deep, 2 m tall, end to end in a line
		Boulder,     // not a plant: a riprap revetment's limestone boulder, an angular block about 1.2 m across, its foot flat
		BareTree,    // a deciduous tree in its winter (sweetgum, maple, hickory, a water oak): an oak's skeleton, bare twigs
		ConcretePole, // not a plant: a square prestressed concrete utility pole, 15 m, its crossarm where the game pole's is
		WinterOak,    // a laurel or water oak in its winter: the oak's model, its leaves yellowed olive and brown (kWinterLeaves)
	};
	constexpr int kSpecies = 16;

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
		kHedgeLeaves, // (a clipped hedge's small leaves)
		kTwigs,       // (a bare tree's: sprays of branching twigs, grey-brown)
		kWinterLeaves, // (the oaks' leaves in a winter's yellowed olive and brown: the laurel and water oaks' in January)
		kPalmFrond, // (a feather frond along each cell: its base at the cell's foot)
		kFanFrond,  // (a cabbage palm's fan: its hub at the cell's foot)
		kBarkOak,
		kBarkPine,
		kBarkMyrtle,
		kBarkSmooth, // (a magnolia's: smooth grey, lenticels)
		kBarkRoyal,   // (a royal palm's: pale grey, faint rings)
		kBarkCoconut, // (grey-brown, ringed)
		kBarkSabal,   // (the boots' criss-cross)
		kCrownshaft,  // (a royal palm's glossy green crownshaft)
		kRock,        // (a boulder's: pale limestone)
		kConcretePole, // (a utility pole's: weathered concrete)
		kTextures,
	};
	texture::Image Picture(Texture t);
	// a picture with an outline (alpha) or a bark (opaque)
	constexpr bool Cutout(Texture t) { return t <= kFanFrond; }

	// The game's photographed pictures our trees are drawn with where one fits: the reviewers saw ours, drawn here, as
	// "leaf cards: oversized dark saturated leaves", "one generic broadleaf model". Its dictionary and name (a cut-out's
	// outline in its alpha), its normal map's (null: flat), how it is cut - cells across and down -, whether its picture
	// is a twig standing on its foot (its cards kept near upright; else any way up), and a bark's metres round and along a
	// limb one picture covers. Null: our own drawn picture (a 2 x 2 atlas, a bark 1.6 m round and 3.2 m along).
	struct GamePicture
	{
		const char *dictionary, *diffuse, *normalDictionary, *normal;
		int across = 1, down = 1;
		bool upright = false;
		float round = 1.6f, along = 3.2f;
		bool rightHalf = false; // (only the picture's right half, made seamless: an atlas's bark beside its foot's)
	};
	const GamePicture* FromGame(Texture t);

	struct Model
	{
		// per level of detail (0 the nearest), per picture: its triangles, positions from the trunk's foot
		std::array<std::array<roads::Mesh, kTextures>, 4> lods;
		float height = 0, width = 0; // its size as made (a placement scales it to the tree's)
		float trunk = 0;             // the trunk's radius at its foot (its collision)
	};
	Model Make(Species s, uint32_t seed);
}
