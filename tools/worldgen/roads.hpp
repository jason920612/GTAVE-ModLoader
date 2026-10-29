#pragma once
#include <array>
#include <cstdint>
#include <functional>
#include <string>
#include <unordered_map>
#include <vector>

#include "paving.hpp"
#include "texture.hpp"
#include "world.hpp"
#include "ydr.hpp"

// Roads from the world description (research/phase0.md §61.16): the network (vertices where roads meet or end, edges
// between them, chains of alike pieces joined), road heights (on the ground; bridges arched over water), the surfaces
// (asphalt strips, junction polygons, raised sidewalks with kerbs and corner pieces, markings with the game's marking
// atlas: US style, double yellow between directions, dashed white between lanes, stop lines), their collision, and the
// traffic graph (path nodes every few metres, junction nodes shared).
namespace worldgen::roads
{
	using P2 = std::array<float, 2>;
	using P3 = std::array<float, 3>;

	struct Edge
	{
		int a = -1, b = -1;       // vertices
		std::vector<P2> points;   // from a to b
		std::vector<float> z;     // the surface height at each point
		int forward = 1, backward = 1; // lanes along a -> b, against it
		int centre = 0;                // a two-way left turn lane between them (lanes)
		float laneWidth = 3.5f;
		bool sidewalks = false, bridge = false;
		int layer = 0; // (OpenStreetMap's: a bridge's level over the ground's roads, 1 and up)
		std::string kind, name;
		int64_t id = 0;                    // the OpenStreetMap way's
		std::string surface, smoothness;   // its tags (paving.hpp)
		float speed = 0; // m/s
		// at its ends (a, b) as OpenStreetMap maps them: a painted crossing; a stop sign for the traffic coming in
		bool crossingA = false, crossingB = false, stopA = false, stopB = false;
		float walkWidth = 2.6f; // its sidewalks' width (m)
		float parking = 0;      // a parking lane each side, this wide (m; 0: none): the traffic lanes between them
		float bike = 0;         // a painted bike lane each side (m; 0: none), between the parking and the traffic lanes
		bool curb = false;      // without sidewalks, a kerb and gutter still (a town's street): a kerb top of kCurbTop
		bool centreLine = false; // a dashed yellow line between the directions where its kind has none (world::Road's)
		// with sidewalks: which sides (left and right of a -> b) have one; the other a kerb and gutter (kCurbTop)
		bool walkLeft = true, walkRight = true;
		float HalfWidth() const { return (forward + backward + centre) * laneWidth / 2 + 0.3f + parking + bike; }
		float Side() const { return parking + bike; }             // (what lies between a kerb and the traffic lanes)
		float LanesHalf() const { return HalfWidth() - Side(); } // (the traffic lanes' half)
		float Walk() const { return sidewalks ? walkWidth : 0.0f; } // (0: none)
	};

	struct Vertex
	{
		P2 p{};
		float z = 0;
		std::vector<int> edges;
		bool signals = false;
	};

	struct Network
	{
		std::vector<Vertex> vertices;
		std::vector<Edge> edges;
		size_t mappedStops = 0, mappedCrossings = 0; // (OpenStreetMap's stop signs and crossings put on edges' ends)
	};

	// A road that is not a street: a driveway, alley or parking lane (service), a track, a path.
	bool Minor(const Edge& e);

	// (its edges have a point at least every kHeightStep m: their heights follow the ground's over the hills)
	constexpr float kHeightStep = 8.0f;
	Network Build(const world::World& w);

	// Each edge the way round from its end lower in the real world's plane (x, then y; worldgen seeds.hpp) to the other - its
	// lanes' and sidewalks' sides turned with it: what is laid out along an edge from its start (the poles, the parked cars,
	// the dashes, a mailbox's side of its driveway) then the same in the two lands that show it; each land's build had
	// turned some of them the other way round.
	void Orient(Network& n);

	// How far round a street junction (three streets or more) its corners reach: the kerbs' curves end where the roads stop
	// short of it (its widest road's half width and sidewalk, a little more), and turning cars straighten out 3 m on; 0
	// at other vertices.
	float CornerReach(const Network& n, int vertex);

	// The surface heights: `ground` (the terrain's height) plus a little. A bridge runs straight between its ends: one on
	// the ground (where the road on it goes on along the ground - on an embankment the measured ground has) at the ground's
	// height, a junction of bridges only in the air at kLevel a layer over the ground (or the sea); one mostly over water
	// between two ends on the ground arches up to `clearance` over its middle (less over a short one); a bridge's deck
	// always 1 m over the ground under it.
	constexpr float kLevel = 6.5f;
	// The steepest a road goes up to a junction of bridges in the air: over lower ground it rises on an embankment between
	// retaining walls (a parapet on them over 1.5 m).
	constexpr float kGrade = 0.07f;
	// The steepest any road goes: the measured ground's cliffs where it took a structure for ground (an airport's decks: a
	// road on them 2.5 m to 9 m in 19 m, 12 m to 3.3 m in 37 m) are ramps on embankments at this instead. Florida's real roads
	// keep under it; a hill's road at less is left as the ground has it.
	constexpr float kSteepest = 0.12f;
	void Heights(Network& n, const std::function<float(float, float)>& ground, float clearance = 6.0f);

	struct Mesh
	{
		std::vector<ydr::Vertex> vertices;
		std::vector<uint32_t> indices;
	};

	struct Collision
	{
		std::vector<P3> vertices;
		std::vector<std::array<uint32_t, 3>> triangles;
		std::vector<uint8_t> materials; // per triangle: a row of materials.dat
		std::vector<uint8_t> kinds;     // per triangle: 1 a carriageway, 2 a sidewalk, 0 other (kerbs)
	};

	// The carriageways' asphalt (the collision's carriageway triangles: roads and junctions) by 8 m cell: whether a point
	// is on it or within `r` of it (r up to the `reach` it was made for).
	class Asphalt
	{
	public:
		Asphalt(const Collision& c, float reach);
		bool Within(float x, float y, float r = 0) const;

	private:
		const Collision& col;
		std::unordered_map<int64_t, std::vector<uint32_t>> cells;
	};

	// Clearances from the asphalt (m): posts (street lights, signal masts) and trees; at the junctions' corners, where turning
	// cars cut across the kerb (the game's AI keeps lanes 4 m wide, wider than ours), more.
	constexpr float kPostClear = 0.3f, kTreeClear = 1.0f, kCornerPostClear = 1.5f, kCornerTreeClear = 2.5f;

	// A traffic signal mast at a signalled junction: where it stands (on the sidewalk), its heading (radians: the model's
	// x axis from east, anticlockwise; its arm, the model's -x, reaches left across the lanes, the heads facing the traffic
	// coming in) and how far the arm has to reach (m).
	struct Signal
	{
		P3 p{};
		float heading = 0;
		float reach = 0;
	};

	struct Surfaces
	{
		std::array<Mesh, paving::kPavings> paved; // the carriageways (and junctions) by what they are made of
		Mesh worn;                                // worn asphalt carriageways (paving::Wear 0.5 and more): cracked all over
		Mesh sidewalk, kerb, markings;
		Mesh sidewalkRed;                         // sidewalks of red-tinted concrete (where the aerial pictures see them red)
		Mesh structure;                           // bridges' decks, parapets and piers (concrete)
		Mesh railTrack, railBed, railSteel;       // railways: the track (sleepers on ballast), the bed's shoulders, the rails
		Mesh potholes, patches, breakups;         // wear's decals over them (paving::Feature)
		Mesh drips;                               // the lanes' oil drips down their middles (a decal: DripPicture)
		Mesh gutters;                             // leaves and grit in the kerbs' gutters (a decal: the game's gutter picture)
		Mesh legends;                             // STOP painted before a stop sign's stop line (StopPicture, a decal)
		Mesh bikeSymbols;                         // a bicycle and an arrow in the bike lanes (BikePicture, a decal)
		std::vector<std::pair<paving::Feature::Kind, P3>> wear; // where each feature is (for checking)
		Collision collision;
		// the ground under the roads: footprints (asphalt and sidewalks) for lowering the terrain below them
		std::vector<std::array<P3, 3>> footprint;
		std::vector<Signal> signals;
	};

	constexpr float kSidewalk = 2.6f, kKerb = 0.15f, kCurbTop = 0.2f;

	// The STOP legend's picture: the four letters side by side, 2.5 times as tall as wide (as they are painted, to be read
	// at a driver's low angle), white; alpha their shapes. u across the lane from the driver's left, v from the letters'
	// tops (away from the driver).
	texture::Image StopPicture();
	// The bike lane symbol's picture: a bicycle (side on, its wheels across the lane) under an arrow ahead, white; alpha their
	// shapes; drawn 1.5 times as tall as on paper (the decal stretches it a little more). u across the lane, v from the
	// arrow's tip (away from the rider).
	texture::Image BikePicture();
	// The lanes' oil drips: a band down a lane's middle (u across its 1.1 m, v along 8 m), dark brown-black; alpha a faint
	// smear thickest in the middle and the drips' blotches on it.
	texture::Image DripPicture();

	// `planted(x, y)`: whether the ground there is planted - a divided road's median that is (a landscaped one) is left as
	// it is; one that is not (or without the function) is paved when it is narrow (research/phase0.md §61.27).
	// `ground(x, y)`: the terrain's height (bridges' piers stand on it; without: none).
	// `pavingRed(x, y)`: the aerial pictures' paving there - its redness (red less the mean of green and blue), NaN where they
	// see none - a sidewalk stretch whose middle they see red (Miami Beach's red concrete) goes into sidewalkRed.
	void Geometry(const Network& n, Surfaces& out, const std::function<bool(float, float)>& planted = {},
	    const std::function<float(float, float)>& ground = {}, const std::function<float(float, float)>& pavingRed = {});

	// Railways (research/phase0.md §63): on the ground a ballast bed 0.35 m high, its track pictured on it (sleepers and
	// ballast) with the rails themselves on top, its shoulders sloping to the ground; elevated (a bridge: an elevated
	// line, the Metrorail's and Metromover's guideways) a concrete beam on piers, at kRailLevel a layer over the ground where
	// a line's bridges meet in the air. Where a road crosses (`onRoad`), the road's surface is there.
	constexpr float kRailLevel = 8.0f;
	void Rails(const std::vector<world::Rail>& rails, const std::function<float(float, float)>& ground, const std::function<bool(float, float)>& onRoad,
	    Surfaces& out);

	// The ways people walk drawn on their own (OpenStreetMap's sidewalks beside the streets, footways, paths, cycleways,
	// steps: research/phase0.md §63) as strips on the ground, 5 cm over it (`ground`: the terrain mesh's height, sampled
	// every metre along), their edges down into it (nothing lowered under them: not in the footprints), the walkers'
	// (navmesh kind 2): sidewalks and footways concrete as the streets' sidewalks, the others of their surface (an
	// untagged path earth, a cycleway asphalt), their width by the map or their kind. Left out: crossings (the road's),
	// footbridges, where a point is on a road (`onRoad`: its asphalt or its own sidewalk) and the pieces `skip` names
	// (per walk, per piece between its points: a street's sidewalk the street draws itself). Returns how many were drawn.
	size_t Walkways(const std::vector<world::Walk>& walks, const std::vector<std::vector<bool>>& skip, const std::function<float(float, float)>& ground,
	    const std::function<bool(float, float)>& onRoad, Surfaces& out);

	// The traffic graph: nodes (junction nodes where three roads or more meet) and links with their lanes.
	struct PathNode
	{
		P3 p{};
		bool junction = false; // (three streets or more meet there, or a signalled junction)
		bool light = false;    // where the cars coming into a signalled junction stop (its traffic light)
		float junctionSize = 0; // a junction's box (m a side: the roads' widths across it)
		float speed = 0;
		uint32_t street = 0; // name hash
		int density = 3;     // vehicles spawned near it (0-15)
		bool off = false;    // switched off: an unnamed service road's (car parks' aisles, an airport's service roads) - no traffic
		                     // made there, the route search keeps to the streets (the game's own car parks and alleys are so)
	};
	struct PathLink
	{
		int a, b;
		int lanesAB, lanesBA;
		float width;    // the road's (m)
		float gap = 0;  // between the directions' lanes (m: a centre turn lane's - the AI keeps off it)
	};
	void Paths(const Network& n, float spacing, std::vector<PathNode>& nodes, std::vector<PathLink>& links);
}
