#pragma once
#include <array>
#include <cstdint>
#include <functional>
#include <string>
#include <vector>

#include "paving.hpp"
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
		float laneWidth = 3.5f;
		bool sidewalks = false, bridge = false;
		std::string kind, name;
		int64_t id = 0;                    // the OpenStreetMap way's
		std::string surface, smoothness;   // its tags (paving.hpp)
		float speed = 0; // m/s
		float HalfWidth() const { return (forward + backward) * laneWidth / 2 + 0.3f; }
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
	};

	Network Build(const world::World& w);

	// The surface heights: `ground` (the terrain's height) plus a little; bridges rise over water (deck at least
	// `clearance` above the sea in the middle).
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
		Mesh potholes, patches, breakups;         // wear's decals over them (paving::Feature)
		std::vector<std::pair<paving::Feature::Kind, P3>> wear; // where each feature is (for checking)
		Collision collision;
		// the ground under the roads: footprints (asphalt and sidewalks) for lowering the terrain below them
		std::vector<std::array<P3, 3>> footprint;
		std::vector<Signal> signals;
	};

	constexpr float kSidewalk = 2.6f, kKerb = 0.15f;

	void Geometry(const Network& n, Surfaces& out);

	// The traffic graph: nodes (junction nodes where three roads or more meet) and links with their lanes.
	struct PathNode
	{
		P3 p{};
		bool junction = false;
		float speed = 0;
		uint32_t street = 0; // name hash
		int density = 3;     // vehicles spawned near it (0-15)
	};
	struct PathLink
	{
		int a, b;
		int lanesAB, lanesBA;
		float width; // the road's (m)
	};
	void Paths(const Network& n, float spacing, std::vector<PathNode>& nodes, std::vector<PathLink>& links);
}
