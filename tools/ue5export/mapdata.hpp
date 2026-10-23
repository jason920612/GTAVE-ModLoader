#pragma once
#include <array>
#include <cstdint>
#include <map>
#include <string>
#include <vector>

#include "gamefiles.hpp"

// The map's metadata (research/phase0.md §58.2, §59.2): archetypes (ytyp: which model and textures; interiors with
// their rooms, portals and entity sets), entities (ymap: where, which archetype, which level of detail), the ymaps
// themselves (parents, extents, grass), and the map data groups the archives' manifests define (the parts of the map
// scripts switch on and off).
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

	using Vec3 = std::array<float, 3>;
	using Quat = std::array<float, 4>; // x y z w, as stored

	struct Entity
	{
		uint32_t archetype = 0;
		Vec3 position{};
		Quat rotation{};
		float scaleXY = 1, scaleZ = 1;
		uint32_t flags = 0;
		uint32_t guid = 0;
		uint32_t lodLevel = 0;
		float lodDist = 0, childLodDist = 0;
		int32_t parentIndex = -1; // in the parent ymap's entities
		uint32_t numChildren = 0;
		uint32_t priorityLevel = 0;
		uint32_t tintValue = 0;
		uint32_t ymap = 0; // index into Map::ymaps
		// A placed interior (CMloInstanceDef).
		bool interior = false;
		uint32_t groupId = 0, floorId = 0, mloFlags = 0;
		std::vector<uint32_t> defaultEntitySets;
	};

	struct Room
	{
		Vec3 bbMin{}, bbMax{};
		uint32_t flags = 0;
		int32_t floorId = 0;
		std::vector<uint32_t> attachedObjects; // indices into the interior's entities
	};

	struct Portal
	{
		uint32_t roomFrom = 0, roomTo = 0, flags = 0, opacity = 0;
		std::vector<Vec3> corners;
		std::vector<uint32_t> attachedObjects;
	};

	struct EntitySet
	{
		uint32_t name = 0;
		std::vector<uint32_t> locations; // a room per entity
		std::vector<Entity> entities;
	};

	struct Interior
	{
		uint32_t mloFlags = 0;
		std::vector<Entity> entities; // in the interior's space
		std::vector<Room> rooms;
		std::vector<Portal> portals;
		std::vector<EntitySet> entitySets;
	};

	struct Archetype
	{
		uint32_t name = 0;
		uint32_t textureDictionary = 0;
		uint32_t drawableDictionary = 0;
		uint32_t assetName = 0;
		AssetType assetType = AssetType::Uninitialized;
		uint32_t flags = 0;
		uint32_t specialAttribute = 0;
		float lodDist = 0, hdTextureDist = 0;
		Vec3 bbMin{}, bbMax{}, bsCentre{};
		float bsRadius = 0;
		bool timed = false; // shown at some hours only
		uint32_t timeFlags = 0;
		int32_t interior = -1; // an MLO: index into Map::interiors
		std::string source;    // the ytyp
	};

	struct GrassInstance
	{
		uint16_t position[3]; // in the batch's box, 0..65535
		uint8_t normalX, normalY;
		uint8_t color[3];
		uint8_t scale;
		uint8_t ao;
		uint8_t pad[3];
	};
	static_assert(sizeof(GrassInstance) == 16);

	struct GrassBatch
	{
		uint32_t archetype = 0;
		Vec3 bbMin{}, bbMax{};
		Vec3 scaleRange{};
		uint32_t lodDist = 0;
		float fadeStart = 0, fadeRange = 0, orientToTerrain = 0;
		std::vector<GrassInstance> instances;
	};

	// A lodlights ymap's light (its position and colour are in the parent distlodlights ymap, same index).
	struct LodLightData
	{
		Vec3 direction{};
		float falloff = 0, falloffExponent = 0;
		uint32_t flags = 0, hash = 0;
		uint8_t coneInner = 0, coneOuter = 0, corona = 0;
	};

	struct DistantLightData
	{
		Vec3 position{};
		uint32_t rgbi = 0;
	};

	struct Ymap
	{
		uint32_t name = 0, parentName = 0;
		int32_t parent = -1; // index into Map::ymaps
		uint32_t flags = 0, contentFlags = 0;
		Vec3 streamingMin{}, streamingMax{}, entitiesMin{}, entitiesMax{};
		uint32_t firstEntity = 0, entityCount = 0;
		uint32_t lodLevels = 0; // a bit per lodLevel its entities have
		std::vector<GrassBatch> grass;
		std::vector<LodLightData> lodLights;         // LODLightsSOA
		std::vector<DistantLightData> distantLights; // DistantLODLightsSOA
		uint32_t lightCategory = 0;
		int32_t group = -1; // index into Map::groups
		std::string file;
	};

	// A map data group (the archives' _manifest.ymf): the ymap of that name, which a script (or the hour, or the
	// weather) switches on.
	struct Group
	{
		uint32_t name = 0, flags = 0, hoursOnOff = 0;
		std::vector<uint32_t> bounds;   // its collision files (name hashes)
		std::vector<uint32_t> weathers; // weather type hashes
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

	struct Map
	{
		std::map<uint32_t, Archetype> archetypes;
		std::vector<Interior> interiors;
		std::vector<Entity> entities; // ymap by ymap
		std::vector<Ymap> ymaps;
		std::map<uint32_t, uint32_t> ymapIndex; // name hash -> index
		std::vector<Group> groups;
		size_t ytypCount = 0, ymapCount = 0;
		std::vector<std::string> errors;

		// The parent of an entity (global index), or -1.
		int64_t Parent(size_t entity) const;
	};

	// Every archetype of the active ytyps, the entities of the active ymaps (all levels of detail), the groups.
	Map Read(const GameFiles& files);
}
