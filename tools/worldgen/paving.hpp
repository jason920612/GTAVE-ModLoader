#pragma once
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

// Road surfaces (research/phase0.md §61.22): what a carriageway is made of (OpenStreetMap surface) and how worn it is
// (smoothness, else its kind), as features that are both seen and felt: the relief its mesh and its collision share
// (potholes' bowls, broken edges, an unpaved road's bumps and ruts), the physics material under each part (the game's
// pothole materials rumble), and the decals drawn over them.
namespace worldgen::paving
{
	enum class Paving : uint8_t { Asphalt, Concrete, Bricks, Setts, Slabs, Gravel, Dirt, Sand };
	constexpr size_t kPavings = 8;

	Paving Of(const std::string& surface);

	// materials.dat rows: the surface's, and where it is broken (the game's pothole materials)
	uint8_t Material(Paving p);
	uint8_t BrokenMaterial(Paving p);
	// how many metres its texture spans
	float TextureMetres(Paving p);
	// lane markings painted on it
	bool Marked(Paving p);
	// unpaved: bumpy all over (its mesh a fine grid)
	bool Rough(Paving p);

	// Wear, 0 new .. 1 falling apart: OSM smoothness when tagged, else by the road's kind (arterials kept up best, service
	// roads and alleys worst), varied from road to road.
	float Wear(const std::string& kind, const std::string& smoothness, int64_t id);

	struct Feature
	{
		enum class Kind : uint8_t { Pothole, Patch, Breakup } kind = Kind::Pothole;
		float s0 = 0, s1 = 0, o0 = 0, o1 = 0; // its box along the road (s) and across it (o: left of the centre line positive)
		float depth = 0;                      // at its deepest (m; patches are flush)
		uint8_t variant = 0;                  // which picture
	};

	// The features of a carriageway between s0 and s1 (its ends kept clear: crossings, stop lines, tapers), `half` wide
	// each side; markings at the offsets `lines` are kept clear (a pothole never under a painted line); `edgeLines`: the
	// road's edges are painted too (no broken edges then).
	std::vector<Feature> Features(Paving p, float wear, uint32_t seed, float s0, float s1, float half, const std::vector<float>& lines, bool edgeLines);

	// The surface's height relative to the flat carriageway at (s, o): the features' and, unpaved, bumps and ruts;
	// nothing within `edge` metres of its sides (the kerbs) or of sStart / sEnd (where junctions join).
	float Relief(Paving p, const std::vector<Feature>& features, uint32_t seed, float s, float o, float half, float sStart, float sEnd);

	// The feature (of those that sink, potholes and broken edges) at (s, o), or nullptr.
	const Feature* BrokenAt(const std::vector<Feature>& features, float s, float o);

	// Where in its picture a feature's box maps to (u0, v0, u1, v1).
	void Picture(const Feature& f, float& u0, float& v0, float& u1, float& v1);

	// How opaque its decal is at (s, o), 0..1: the game's pothole and crumbled-edge pictures are opaque all over (the
	// game's own decal meshes fade them out with their vertices' alpha), so a pothole's fades from its rim to its box's
	// edge, a broken edge's at its ends and towards the road's middle; a patch has hard edges.
	float Fade(const Feature& f, float s, float o, float half);
}
