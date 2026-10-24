#pragma once
#include <array>
#include <cstdint>
#include <map>
#include <optional>
#include <string>
#include <vector>

#include "convert/resource.hpp"

// Enhanced drawables (ydr; ydd dictionaries; the main drawable of a yft) and texture dictionaries, read for the
// export (research/phase0.md §15, §16, §58.4): geometry of the highest level of detail, the shaders' textures and
// constants, embedded textures.
namespace ue5export::drawable
{
	// A pointer of the resource -> bytes in its virtual (0x5...) or physical (0x6...) block.
	class View
	{
	public:
		explicit View(const loader::convert::Resource& r) : r_(r) {}
		const uint8_t* At(uint64_t pointer, size_t size) const;
		template <typename T>
		T Get(uint64_t pointer) const
		{
			T value{};
			if (const uint8_t* p = At(pointer, sizeof(T)))
				std::memcpy(&value, p, sizeof(T));
			return value;
		}
		std::string String(uint64_t pointer) const;
		static constexpr uint64_t kRoot = 0x50000000;

	private:
		const loader::convert::Resource& r_;
	};

	struct Texture
	{
		std::string name; // as stored (case kept)
		uint16_t width = 0, height = 0, depth = 1;
		uint8_t mips = 1;
		uint8_t dxgi = 0;
		std::vector<uint8_t> data; // the mip chain, tightly packed (as a DDS wants it)
	};

	struct Material
	{
		uint32_t effect = 0;
		std::map<uint32_t, std::string> textures;              // parameter name hash -> texture name
		std::map<uint32_t, std::vector<float>> constants;      // parameter name hash -> values
		uint8_t drawBucket = 0;
	};

	struct Section
	{
		uint32_t material = 0;
		uint8_t lod = 0; // the drawable's level of detail (0 high, 1 medium, 2 low, 3 very low)
		uint32_t vertices = 0;
		std::vector<float> position, normal, tangent; // 3, 3, 4 per vertex (normal / tangent empty when absent)
		std::vector<uint8_t> color0, color1;          // RGBA8 per vertex, or empty
		std::vector<std::vector<float>> uv;           // per set: 2 per vertex
		std::vector<uint32_t> indices;
		std::vector<float> flow; // a stream's own current at each vertex (x, y: the game's), when it has one
	};

	// A light attribute of a drawable (0xA8 bytes each; research/phase0.md §59.9), in the game's space (metres).
	struct Light
	{
		std::array<float, 3> position{}, direction{}, tangent{};
		std::array<uint8_t, 3> color{};
		uint8_t type = 0; // 1 point, 2 spot, 4 capsule
		uint16_t bone = 0;
		float intensity = 0, falloff = 0, falloffExponent = 0, coneInner = 0, coneOuter = 0;
		float length = 0; // a capsule's (m), along the tangent
		float coronaSize = 0, coronaIntensity = 0; // (m; 0: none)
		uint32_t timeFlags = 0, flags = 0;
	};

	struct Model
	{
		std::vector<Section> sections;
		std::vector<Material> materials;
		std::vector<Texture> embedded;
		std::array<float, 3> bbMin{}, bbMax{};
		std::array<float, 4> sphere{};       // centre, radius
		std::array<float, 4> lodDistances{}; // per level of detail
		uint8_t lodCount = 0;
		std::vector<Light> lights;
		std::vector<std::string> warnings;
	};

	// The drawable at `at` (a pointer into the resource): every level of detail it has (allLods) or the highest.
	bool Read(const View& view, uint64_t at, Model& out, std::string& error, bool allLods = false);
	// The light attributes of an atArray at `field`: a drawable's (+0xB0) or a fragment's (+0x110), placed in the model
	// by the bones they hang on (the skeleton of the drawable at `drawable`, in its rest pose).
	std::vector<Light> Lights(const View& view, uint64_t field, uint64_t drawable);
	// Drawables of a resource by kind: ydr -> one (hash 0), ydd -> name hash -> drawable, yft -> its main drawable.
	std::map<uint32_t, uint64_t> Drawables(const View& view, const std::string& extension);
	// The textures of a dictionary (ytd root, or a drawable's shader group dictionary).
	std::vector<Texture> Textures(const View& view, uint64_t dictionary);
}
