#pragma once
#include <array>
#include <cstdint>
#include <map>
#include <string>
#include <vector>

#include "gamefiles.hpp"

// The map's metadata (research/phase0.md §58.2): archetypes (ytyp: which model and textures) and entities (ymap:
// where, which archetype, which level of detail).
namespace ue5export::mapdata
{
	enum class AssetType : uint8_t
	{
		Uninitialized,
		Fragment,
		Drawable,
		DrawableDictionary,
		Assetless,
	};

	struct Archetype
	{
		uint32_t name = 0;
		uint32_t textureDictionary = 0;
		uint32_t drawableDictionary = 0;
		uint32_t assetName = 0;
		AssetType assetType = AssetType::Uninitialized;
		uint32_t flags = 0;
		float lodDist = 0;
		std::array<float, 3> bbMin{}, bbMax{};
		bool interior = false; // an MLO (rooms, portals, its own entities)
		bool timed = false;    // shown at some hours only
		std::string source;    // the ytyp
	};

	// Entity lodLevel values.
	enum LodLevel : uint32_t
	{
		kHd = 0,
		kLod = 1,
		kSlod1 = 2,
		kSlod2 = 3,
		kSlod3 = 4,
		kOrphanHd = 5,
		kSlod4 = 6,
	};

	struct Entity
	{
		uint32_t archetype = 0;
		std::array<float, 3> position{};
		std::array<float, 4> rotation{}; // x y z w, as stored
		float scaleXY = 1, scaleZ = 1;
		uint32_t flags = 0;
		uint32_t lodLevel = 0;
		float lodDist = 0;
		int32_t parentIndex = -1;
		bool interior = false; // a CMloInstanceDef
		uint32_t ymap = 0;     // name hash of its ymap
	};

	struct Map
	{
		std::map<uint32_t, Archetype> archetypes;
		std::vector<Entity> entities;
		std::map<uint32_t, std::string> ymapNames;
		size_t ytypCount = 0, ymapCount = 0;
		std::vector<std::string> errors;
	};

	// Every archetype of the active ytyps, and the entities of the active ymaps (all levels of detail).
	Map Read(const GameFiles& files);
}
