#pragma once
// The graphics mod's pack files (research/phase0.md §59): the game's models, textures and map, converted once by
// tools/ue5export from the player's own game files and read by the renderer at run time. Plain C++ (shared by the
// exporter and the Unreal module), little-endian, no padding surprises: every struct is a multiple of 4 bytes.
//
// A pack file: FileHeader, the entries' data (each 16-byte aligned), then Entry[entryCount] sorted by key.
#include <cstdint>

namespace GTAVPack
{
	constexpr uint32_t kVersion = 2;

	enum Kind : uint32_t
	{
		kModels = 1,   // key: joaat of the model's name (the archetype's name for map models)
		kTextures = 2, // key: the texture's content hash
		kWorld = 3,    // key: a WorldSection
	};

	struct FileHeader
	{
		char magic[4]; // "GPK1"
		uint32_t kind;
		uint32_t version;
		uint32_t entryCount;
		uint64_t tableOffset;
		uint64_t reserved[3];
	};
	static_assert(sizeof(FileHeader) == 48);

	struct Entry
	{
		uint64_t key;
		uint64_t offset;
		uint32_t size;
		uint32_t flags;
	};
	static_assert(sizeof(Entry) == 24);

	// --- textures ---------------------------------------------------------------------------------------------
	// TextureHeader, then the mip chain as the game has it: largest first, tightly packed (block-compressed formats
	// in 4x4 blocks). The game's DXGI format: 71 BC1, 74 BC2, 77 BC3, 80 BC4, 83 BC5, 98 BC7, 87 B8G8R8A8,
	// 28 R8G8B8A8, 65 A8.
	struct TextureHeader
	{
		uint16_t width, height;
		uint8_t mips;
		uint8_t dxgi;
		uint16_t flags;
	};
	static_assert(sizeof(TextureHeader) == 8);

	// --- models -----------------------------------------------------------------------------------------------
	// ModelHeader, Material[materialCount], then per section a SectionHeader and its data. Everything is in Unreal's
	// space already (centimetres, y mirrored; tangent w = Unreal's binormal sign).
	struct ModelHeader
	{
		char magic[4]; // "GMDL"
		uint32_t version;
		float boundsMin[3], boundsMax[3];
		float sphere[4];       // centre, radius
		float lodDistances[4]; // the drawable's own levels of detail (metres; sections say which level they are)
		uint16_t materialCount, sectionCount;
		uint8_t lodCount;
		uint8_t reserved[3];
		uint32_t flags; // ModelFlags
	};
	static_assert(sizeof(ModelHeader) == 76);

	enum ModelFlags : uint32_t
	{
		kModelLights = 1, // after the sections: uint32 count, ModelLight[count]
	};

	// A light the model carries (the drawable's light attributes; research/phase0.md §59.9), in the model's space.
	struct ModelLight
	{
		float position[3];  // Unreal's space (cm)
		float direction[3]; // unit, Unreal's space
		uint8_t color[3];
		uint8_t type;       // the game's: 1 point, 2 spot, 4 capsule
		float intensity;    // the game's
		float falloff;      // cm
		float falloffExponent;
		float coneInner, coneOuter; // degrees
		uint32_t timeFlags; // bits 0-23: the hours it is on
		uint32_t flags;
	};
	static_assert(sizeof(ModelLight) == 56);

	// What a material draws like (the renderer's master materials).
	enum MaterialKind : uint8_t
	{
		kOpaque = 0,
		kTranslucent = 1,
		kDecal = 2,
		kCutout = 3,
		kTerrain = 4,
		kWater = 6,
	};

	// The shader's texture parameters the renderer uses.
	enum Slot : uint8_t
	{
		kDiffuse,
		kBump,
		kSpecular,
		kDetail,
		kTintPalette,
		kLayerDiffuse0, // terrain: DiffuseTexture_layer0..3
		kLayerDiffuse1,
		kLayerDiffuse2,
		kLayerDiffuse3,
		kLayerBump0, // BumpTexture_layer0..3
		kLayerBump1,
		kLayerBump2,
		kLayerBump3,
		kHeight,
		kDirt,
		kSlotCount,
	};

	struct Material
	{
		uint32_t effect; // the game's shader (name hash)
		uint8_t bucket;  // the game's draw bucket
		uint8_t kind;    // MaterialKind
		uint16_t flags;
		uint64_t textures[kSlotCount]; // content hashes (0: none)
		float tintRow;                 // tintPaletteSelector
		float emissive;                // emissiveMultiplier
		float bumpiness;
		float specularIntensity;
		float color[4];  // water: its colour
		float detail[4]; // detailSettings
		float alphaTest;
		float reserved[3];
	};
	static_assert(sizeof(Material) % 8 == 0);

	enum SectionFlags : uint8_t
	{
		kNormals = 1,
		kTangents = 2,
		kColor0 = 4,
		kColor1 = 8,
		kIndex32 = 16,
	};

	// Then: float position[3] x vertices; [int8 normal[4]]; [int8 tangent[4]]; [uint8 colour0[4]]; [uint8 colour1[4]];
	// float uv[2] x vertices per UV set; indices (uint16 or uint32), padded to 4 bytes.
	struct SectionHeader
	{
		uint16_t material;
		uint8_t uvCount;
		uint8_t flags;
		uint32_t vertexCount;
		uint32_t indexCount;
		uint8_t lod; // 0: the most detailed
		uint8_t reserved[3];
	};
	static_assert(sizeof(SectionHeader) == 16);

	// --- world ------------------------------------------------------------------------------------------------
	// One entry per section, each an array of one of the structs below. Positions, rotations and boxes are in Unreal's
	// space (centimetres, y mirrored) unless said otherwise. "None" indices are 0xFFFFFFFF.
	enum WorldSection : uint64_t
	{
		kPlacements = 1,       // Placement[]: a test area's models as placed (old test packs)
		kArchetypes = 2,       // Archetype[], sorted by name
		kYmaps = 3,            // Ymap[]
		kEntities = 4,         // Entity[], ymap by ymap (every level of detail)
		kGroups = 5,           // Group[]: map data groups (what scripts switch on)
		kInteriors = 6,        // Interior[]: interior (MLO) archetypes
		kInteriorEntities = 7, // InteriorEntity[]: their entities, in the interior's space
		kRooms = 8,            // Room[]
		kPortals = 9,          // Portal[]
		kEntitySets = 10,      // EntitySet[]
		kInteriorPlacements = 11, // InteriorPlacement[]: interiors as placed (CMloInstanceDef entities)
		kGrassBatches = 12,    // GrassBatch[]
		kGrassInstances = 13,  // GrassInstance[] (the game's own 16 bytes)
		kLists = 14,           // uint32[]: the variable-length lists the others point into
		kWaterQuads = 15,      // WaterQuad[]: the sea, lakes, rivers (water.xml)
		kCalmingQuads = 16,    // CalmingQuad[]
		kWaveQuads = 17,       // WaveQuad[]
		kLodLights = 18,       // LodLight[]: the map's lights as the game has them for the distance (lodlights ymaps)
	};
	constexpr uint32_t kNone = 0xFFFFFFFF;

	enum ArchetypeKind : uint8_t
	{
		kBase = 0,
		kTimed = 1,    // shown at some hours only (timeFlags)
		kInterior = 2, // an MLO
	};

	struct Archetype
	{
		uint32_t name;
		uint32_t flags;
		float lodDist;      // metres (the game's)
		float hdTextureDist;
		float boundsMin[3], boundsMax[3];
		float sphere[4]; // centre, radius
		uint32_t timeFlags;
		uint32_t interior; // index into kInteriors
		uint32_t specialAttribute;
		uint8_t kind;      // ArchetypeKind
		uint8_t hasModel;  // in models.gpk under its name
		uint8_t reserved[2];
	};
	static_assert(sizeof(Archetype) == 72);

	struct Ymap
	{
		uint32_t name;
		uint32_t parent; // ymap index
		uint32_t flags, contentFlags;
		float streamingMin[3], streamingMax[3];
		float entitiesMin[3], entitiesMax[3];
		uint32_t firstEntity, entityCount;
		uint32_t firstGrass, grassCount;
		uint32_t group; // group index: the ymap is shown only while its group is on
		uint32_t reserved;
	};
	static_assert(sizeof(Ymap) == 88);

	struct Entity
	{
		uint32_t archetype; // name hash
		uint32_t ymap;
		float position[3];
		float rotation[4];
		float scaleXY, scaleZ;
		float lodDist, childLodDist; // metres
		uint32_t parent;             // entity index (the next level of detail out)
		uint32_t flags;
		uint32_t tintValue;
		uint32_t interiorPlacement; // index into kInteriorPlacements for a placed interior
		uint8_t lodLevel;           // 0 HD, 1 LOD, 2-4 SLOD1-3, 5 orphan HD, 6 SLOD4
		uint8_t priorityLevel;
		uint16_t numChildren;
	};
	static_assert(sizeof(Entity) == 72);

	struct Group
	{
		uint32_t name;
		uint32_t flags;
		uint32_t hoursOnOff; // bit per hour, 0: not by the hour
		uint32_t ymap;       // the ymap it switches
		uint32_t firstWeather, weatherCount; // into kLists (weather type hashes)
	};
	static_assert(sizeof(Group) == 24);

	struct Interior
	{
		uint32_t archetype;
		uint32_t mloFlags;
		uint32_t firstEntity, entityCount; // into kInteriorEntities
		uint32_t firstRoom, roomCount;
		uint32_t firstPortal, portalCount;
		uint32_t firstSet, setCount;
	};
	static_assert(sizeof(Interior) == 40);

	struct InteriorEntity
	{
		uint32_t archetype;
		float position[3]; // in the interior's space
		float rotation[4];
		float scaleXY, scaleZ;
		float lodDist;
		uint32_t flags;
		uint32_t tintValue;
		uint8_t lodLevel;
		uint8_t reserved[3];
	};
	static_assert(sizeof(InteriorEntity) == 56);

	struct Room
	{
		float boundsMin[3], boundsMax[3]; // interior space
		uint32_t flags;
		int32_t floorId;
		uint32_t firstObject, objectCount; // into kLists: entity indices relative to the interior's first entity
	};
	static_assert(sizeof(Room) == 40);

	struct Portal
	{
		uint32_t roomFrom, roomTo, flags, opacity;
		float corners[4][3]; // interior space
		uint32_t firstObject, objectCount;
	};
	static_assert(sizeof(Portal) == 72);

	struct EntitySet
	{
		uint32_t name;
		uint32_t firstEntity, entityCount;     // into kInteriorEntities
		uint32_t firstLocation, locationCount; // into kLists: the room of each entity
	};
	static_assert(sizeof(EntitySet) == 20);

	struct InteriorPlacement
	{
		uint32_t entity;   // the placing entity
		uint32_t interior; // index into kInteriors (kNone: archetype not found)
		uint32_t groupId, floorId, flags;
		uint32_t firstDefaultSet, defaultSetCount; // into kLists: entity set names on by default
	};
	static_assert(sizeof(InteriorPlacement) == 28);

	struct GrassBatch
	{
		uint32_t archetype;
		uint32_t ymap;
		float boundsMin[3], boundsMax[3]; // the GAME's space (metres): instance positions are fractions of it
		float scaleRange[3];
		float lodDist, fadeStart, fadeRange, orientToTerrain;
		uint32_t firstInstance, instanceCount;
	};
	static_assert(sizeof(GrassBatch) == 68);

	struct GrassInstance
	{
		uint16_t position[3]; // in the batch's box, 0..65535
		uint8_t normalX, normalY;
		uint8_t color[3];
		uint8_t scale;
		uint8_t ao;
		uint8_t reserved[3];
	};
	static_assert(sizeof(GrassInstance) == 16);

	// Water (the GAME's space, metres): a box at height z, or a right triangle of it (type 1-4: which corner is left
	// out), with the corners' alpha.
	enum WaterFlags : uint8_t
	{
		kInvisible = 1,
		kLimitedDepth = 2,
		kNoStencil = 4,
	};
	struct WaterQuad
	{
		float minX, minY, maxX, maxY;
		float z;
		uint8_t type;
		uint8_t flags;
		uint8_t alpha[4];
		uint8_t reserved[2];
	};
	static_assert(sizeof(WaterQuad) == 28);

	// One of the map's LOD lights (street lights, windows, signs): what the game draws in the distance at night, and as
	// light near them. The lodlights ymaps hold these, their parents (distlodlights) the positions and colours.
	struct LodLight
	{
		float position[3];  // Unreal's space (cm)
		float direction[3]; // unit, Unreal's space (spot lights)
		float falloff;      // cm
		float falloffExponent;
		uint32_t rgbi;      // the game's RGBI
		uint32_t flags;     // the game's timeAndStateFlags: bits 0-23 the hours it is on
		uint32_t hash;
		uint8_t coneInner, coneOuter; // the game's bytes
		uint8_t corona;               // corona intensity
		uint8_t category;             // the distant set's
	};
	static_assert(sizeof(LodLight) == 48);

	struct CalmingQuad
	{
		float minX, minY, maxX, maxY;
		float dampening;
	};
	static_assert(sizeof(CalmingQuad) == 20);

	struct WaveQuad
	{
		float minX, minY, maxX, maxY;
		float amplitude, xDirection, yDirection;
	};
	static_assert(sizeof(WaveQuad) == 28);

	struct Placement
	{
		uint64_t model;
		float position[3]; // Unreal space
		float rotation[4]; // Unreal quaternion x y z w
		float scale[3];
		uint32_t flags;
		uint32_t reserved;
	};
	static_assert(sizeof(Placement) == 56);
}
