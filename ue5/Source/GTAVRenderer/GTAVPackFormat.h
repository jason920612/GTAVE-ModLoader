#pragma once
// The graphics mod's pack files (research/phase0.md §59): the game's models, textures and map, converted once by
// tools/ue5export from the player's own game files and read by the renderer at run time. Plain C++ (shared by the
// exporter and the Unreal module), little-endian, no padding surprises: every struct is a multiple of 4 bytes.
//
// A pack file: FileHeader, the entries' data (each 16-byte aligned), then Entry[entryCount] sorted by key.
#include <cstdint>

namespace GTAVPack
{
	constexpr uint32_t kVersion = 1;

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
		uint16_t materialCount, sectionCount;
		uint32_t flags;
	};
	static_assert(sizeof(ModelHeader) == 40);

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
	};
	static_assert(sizeof(SectionHeader) == 12);

	// --- world ------------------------------------------------------------------------------------------------
	enum WorldSection : uint64_t
	{
		kPlacements = 1, // Placement[]: a test area's models as placed (ue5export pack --area)
	};

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
