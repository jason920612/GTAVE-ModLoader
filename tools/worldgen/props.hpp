#pragma once
#include <array>
#include <cstdint>
#include <functional>
#include <vector>

#include "roads.hpp"
#include "world.hpp"

// The street's furniture and trees (research/phase0.md §61.17), the game's own props: palms at the world's tree
// points and along the avenues, street lights along roads with sidewalks (their arm over the road), traffic signal
// masts at the signalled junctions (where roads::Geometry puts them: the far right corner of each road coming in).
namespace worldgen::props
{
	using P2 = std::array<float, 2>;
	using P3 = std::array<float, 3>;

	struct Prop
	{
		uint32_t archetype = 0;
		P3 position{};
		float heading = 0; // radians: the model's x axis from east, anticlockwise
		float lodDist = 200;
		float scale = 1;
		float scaleZ = 0; // (0: as scale; a tree the aerial pictures measured: its height's, scale its crown's)
	};

	// `open(x, y)`: not on a road's asphalt or in a building; `ground(x, y)`: the terrain's height. Only what lies in
	// `box` (x0, y0, x1, y1). `palms`: the world's tree points and the avenues' palms (off when a laser scan places the
	// trees where they are).
	void Place(const world::World& w, const roads::Network& n, const std::vector<roads::Signal>& signals, const std::function<bool(float, float)>& open,
	    const std::function<float(float, float)>& ground, const std::array<float, 4>& box, std::vector<Prop>& out, bool palms = true);

	// A cemetery's graves (research/phase0.md §63.10): the game's headstones, raised tombs, monuments, an obelisk and now
	// and then a mausoleum, in rows along the outline's longest straight run, their faces the most easterly way across
	// the rows - patches of them, some lawn between -; none in a memorial park's (its name says so: flat markers in the
	// lawn). None on its paths (`walks`: the map's ways people walk), on a road or in a building (`open`), within 1.2 m of
	// its edge or by a prop already in `out` (its trees and bushes).
	// Whether a cemetery's name is a memorial park's, a national cemetery's or a columbarium's (flat markers in the lawn
	// or niches in a wall: no rows of upright stones, no fence).
	bool MemorialPark(const std::string& name);
	struct GraveStats
	{
		size_t cemeteries = 0, graves = 0;
	};
	void Graves(const std::vector<world::Area>& areas, const std::vector<world::Walk>& walks, const std::function<bool(float, float)>& open,
	    const std::function<float(float, float)>& ground, const std::array<float, 4>& box, std::vector<Prop>& out, GraveStats& stats);
}
