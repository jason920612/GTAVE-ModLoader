#pragma once
#include <array>
#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

#include "world.hpp"

// Editing the road network the way city builders do (research/phase0.md §61.19; the worldedit mod and worldgen share
// it): a stroke - straight or curved - becomes a road of a chosen type; its ends snap to roads' points or onto their
// pieces, and wherever it meets or crosses a road both get a shared point there (roads::Build makes a junction of every
// shared point). Deleting takes the piece of road between two junctions. Every change can be undone. The world itself
// stays as imported: the changes are kept as edits (edits.json: the roads added or changed, by id, and those removed),
// which worldgen applies before it builds (world::ApplyEdits).
namespace worldgen::roadedit
{
	using P2 = world::P2;

	// A road type to draw with (city-builder presets).
	struct Type
	{
		const char* id;
		const char* label;
		const char* kind; // OSM highway value
		int forward, backward;
		float laneWidth;
		bool sidewalks;
	};
	const std::vector<Type>& Types();

	struct Snap
	{
		enum class Kind : uint8_t { None, Point, Piece } kind = Kind::None;
		size_t road = 0, segment = 0; // the road (index) and, for a piece, the segment the point lies on
		P2 at{};
		float distance = 0;
	};

	// Roads that change: their new versions (a new road has a new id), and roads that go.
	struct Change
	{
		std::vector<world::Road> roads;
		std::vector<int64_t> removed;
		bool Empty() const { return roads.empty() && removed.empty(); }
	};

	class Network
	{
	public:
		// The roads (worldgen roads-index: roads.bin, as imported) and the edits made so far (edits.json; missing: none).
		bool Load(const std::filesystem::path& index, const std::filesystem::path& edits, std::string& error);
		bool SaveEdits(const std::filesystem::path& file, std::string& error) const;

		const std::vector<world::Road>& Roads() const { return roads_; }
		const world::Road* Find(int64_t id) const;
		// roads with a point within the box (deleted ones not)
		std::vector<size_t> Near(float x0, float y0, float x1, float y1) const;

		// The nearest road point within `radius`, else the nearest point on a road's piece within it.
		Snap SnapTo(P2 p, float radius) const;
		// A stroke (its points; the first and last as snapped) of a type: the new road and the roads it meets.
		Change Plan(const std::vector<P2>& stroke, const Type& type, const Snap& start, const Snap& end) const;
		// The piece of road (between two junctions) nearest p within `radius`, removed.
		Change PlanDelete(P2 p, float radius) const;
		void Commit(const Change& c);
		bool Undo();
		size_t Edits() const { return edited_.size() + removed_.size(); }

		// A quadratic curve (start, control, end) as points about every `step` metres.
		static std::vector<P2> Curve(P2 a, P2 control, P2 b, float step = 4.0f);

	private:
		void Index(size_t road);
		void Unindex(size_t road);
		bool Shared(const P2& p, size_t except) const; // another road has this point
		void Apply(const world::Road& r);

		std::vector<world::Road> roads_; // (removed ones: no points)
		std::unordered_map<int64_t, size_t> byId_;
		std::unordered_map<uint64_t, std::vector<size_t>> grid_; // 64 m cells -> roads with a point or piece there
		std::unordered_map<int64_t, int> edited_;                // ids added or changed (value unused)
		std::unordered_map<int64_t, int> removed_;               // original ids removed
		std::unordered_map<int64_t, int> original_;              // ids of the imported roads
		struct Step
		{
			std::vector<std::pair<int64_t, std::optional<world::Road>>> before; // each road's state before (none: it was new)
			std::vector<std::pair<int64_t, bool>> wasEdited, wasRemoved;
		};
		std::vector<Step> history_;
		int64_t nextId_ = 9000000000000000;
	};
}
