#pragma once
#include <array>
#include <functional>
#include <string>
#include <vector>

#include "elevation.hpp"
#include "props.hpp"

// Trees where the laser scan saw them (research/phase0.md §61.18): the canopy's height over the ground (vegetation
// returns, outside the buildings) smoothed, its peaks (each the highest within a reach that grows with its height) are
// the trees; how far the crown falls to half the peak's height is its radius. A narrow crown for its height is a palm,
// a wide one a broadleaf tree (Miami's figs, oaks, mahoganies); each becomes one of the game's models of the nearest
// height, scaled to the measured one; low growth becomes bushes.
namespace worldgen::trees
{
	struct Stats
	{
		size_t palms = 0, broadleaf = 0, bushes = 0;
	};

	// `open(x, y)`: a trunk may stand there (not on a road, not in a building); `ground(x, y)`: the terrain's height.
	void Place(elevation::Scan& scan, const std::function<bool(float, float)>& open, const std::function<float(float, float)>& ground,
	    const std::array<float, 4>& box, std::vector<props::Prop>& out, Stats& stats);

	// Where no laser scan says where the trees stand (a land built from the map alone, as Florida's sectors are): on a
	// lattice of 22 m (jittered; every entity counts against the game's pools), a tree by what covers the ground there -
	// `cover(x, y)`, its NLCD class (tools/worldgen/nlcd.py; 0 or no function: not known, the map's woods and wetlands
	// then): forests every point (evergreen: pines - south of 27 N slash pines with palms; deciduous: oaks - south of it
	// with figs and palms), woody wetlands 70 % (cypresses, palms, shrubs), scrub 35 % (shrubs, palmettos), built-up
	// land 20 % down to 2 % by how built up (yard trees), pastures a lone oak now and then.
	void Fill(const std::vector<world::Area>& areas, const std::function<uint8_t(float, float)>& cover, const std::function<double(float)>& latitudeAt,
	    const std::function<bool(float, float)>& open, const std::function<float(float, float)>& ground, const std::array<float, 4>& box,
	    std::vector<props::Prop>& out, Stats& stats);

	// A tree the aerial pictures show (tools/worldgen/photo.py, research/phase0.md §63): where it stands, how tall it is
	// (the canopy heights), its crown's radius, and its kind - 0 green in the pictures' winter (pines, live oaks), 1
	// leafless then (the broadleaf trees that drop their leaves), 2 a palm's small round crown.
	struct Seen
	{
		float x = 0, y = 0, height = 0, crown = 0;
		uint8_t kind = 0;
	};

	// Every tree the pictures show as one of the game's models of its kind nearest in height and crown, scaled to them
	// (height and crown apart, within what keeps a trunk a trunk; crowns a third wider: the canopy closes as in the
	// pictures): the evergreens as pines of every age or, with a crown wide for their height, live oaks; south of 27.5 N
	// some of them cabbage palms; the leafless as maples and oaks by their size. `cover`: the NLCD class.
	// Trees of our own (flora.hpp) to use instead of the game's: per kind - 0 live oak, 1 pine, 2 oak, 3 crape myrtle, 4
	// magnolia, 5 sweetgum, 6 royal palm, 7 coconut palm, 8 cabbage palm, 9 areca palm, 10 a big shrub (a town's low evergreen
	// crowns: its yards' hedge masses), 11 a bare deciduous tree (the north's leafless crowns in the pictures' winter) - the
	// models (their archetypes' names, heights and widths as made); a kind the land has none of: the game's.
	struct Own
	{
		std::string name;
		float height = 0, width = 0;
	};
	using OwnTrees = std::array<std::vector<Own>, 12>;

	// (`own`: north Florida's trees of our own in place of the game's California pines and English oaks, where given)
	void Photo(const std::vector<Seen>& seen, const std::function<uint8_t(float, float)>& cover, const std::function<double(float)>& latitudeAt,
	    const std::function<bool(float, float)>& open, const std::function<float(float, float)>& ground, const std::array<float, 4>& box,
	    std::vector<props::Prop>& out, Stats& stats, const OwnTrees* own = nullptr);

	// Undergrowth (the street pictures' wooded lots: a wall of shrubs and vines under the trees, nothing seen through):
	// large shrubs on a 6 m lattice (jittered, seven in ten) where `where(x, y)` - the woods by the streets: every entity
	// counts against the game's pools, and only there is it seen.
	// (`step`: the lattice, `share`: of ten the points with a bush, its size `smallest` to `largest` of the model's)
	void Undergrowth(const std::function<bool(float, float)>& where, const std::function<bool(float, float)>& open,
	    const std::function<float(float, float)>& ground, const std::array<float, 4>& box, std::vector<props::Prop>& out, Stats& stats,
	    float step = 6.0f, uint32_t share = 7, float smallest = 0.8f, float largest = 1.4f);

	// The scan's low growth (hedges, shrubs: 2-3.5 m over the ground - a 2020 scan that classes no vegetation shows nothing
	// lower) where `where(x, y)`: a shrub 3 m across on a 3 m lattice, scaled to the height.
	// `hedges`: clipped hedge pieces of our own (flora Hedge, 2.6 m long along their y, 2 m tall): where the low growth runs
	// in a line - each with a neighbour on either side of it, or one - a piece along the line, scaled to the height.
	void LowGrowth(elevation::Scan& scan, const std::function<bool(float, float)>& where, const std::function<bool(float, float)>& open,
	    const std::function<float(float, float)>& ground, const std::array<float, 4>& box, std::vector<props::Prop>& out, Stats& stats,
	    const std::vector<Own>* hedges = nullptr);

	// The trees the scan saw in the box (its canopy's peaks, as Place finds them): where they stand, how tall, their crowns'
	// radii; their kind left 0 (the caller's to set - the pictures', which tell a pine from a palm).
	std::vector<Seen> Detect(elevation::Scan& scan, const std::function<float(float, float)>& ground, const std::array<float, 4>& box);

	// The game's own trees, palms and bushes placed here: their models carry collision of their own, a physics instance each
	// - thousands in a town, all put into the physics' broad phase in the frame a place's maps come in (research/phase0.md
	// §66.2). Whether `archetype` is one, and its trunk at scale 1 (radius, how high its collision goes; a bush 0): placed
	// with the map entity's flag not to instance its collision, a tree's trunk is the tile's collision instead.
	bool GameFlora(uint32_t archetype, float& radius, float& height);
}
