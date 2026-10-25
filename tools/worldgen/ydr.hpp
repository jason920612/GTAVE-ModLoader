#pragma once
#include <array>
#include <cstdint>
#include <string>
#include <utility>
#include <vector>

#include "../ue5export/drawable.hpp"
#include "convert/resource.hpp"

// Writing models (.ydr, the Enhanced form, version 159; research/phase0.md §15, §16, §61.12) from scratch: one model
// per level of detail, a geometry per mesh, shaders that name their parameters (the game matches them to the
// effect's layout by name, as the legacy converter's do), textures by name (found in the archetype's texture
// dictionary and its parents). Every pointer is written where it is known, never guessed from the data.
//   Drawable (0xD0): +0x08 page map, +0x10 shader group, +0x20 sphere, +0x30 / +0x40 box (w 0x7F800001), +0x50 the
//   four levels' model lists ({models, u16 count, capacity}), +0x70 their distances, +0x80 1, 0xFF, +0xA8 name.
//   Shader group (0x40): +0x10 the shaders, u16 count, capacity. Model (0x30): +0x08 geometries, u16 count, capacity,
//   +0x18 boxes (32 bytes: all, then each geometry's; one when one geometry), +0x20 each geometry's shader (u16),
//   +0x2C 0xFF (drawn in every pass), +0x2E the geometry count. Geometry (0xA0): +0x18 vertex buffer, +0x38 index
//   buffer, +0x58 index count, +0x5C triangles, +0x60 u16 vertices, u16 3, +0x70 stride, +0x78 the vertex data.
namespace worldgen::ydr
{
	// Vertex elements: the slot of the Enhanced declaration.
	enum Slot : uint8_t
	{
		kPosition = 0,
		kNormal = 4,
		kTangent = 8,
		kColour0 = 24,
		kColour1 = 25,
		kUv0 = 28,
		kUv1 = 29,
		kUv2 = 30,
		kUv3 = 31,
	};

	struct Vertex
	{
		std::array<float, 3> position{}, normal{0, 0, 1};
		std::array<float, 4> tangent{1, 0, 0, 1};
		std::array<uint8_t, 4> colour0{255, 255, 255, 255}, colour1{0, 0, 0, 255};
		std::array<std::array<float, 2>, 4> uv{};
	};

	// A shader: its effect, textures by parameter name hash, constants by parameter (their bytes), and the vertex
	// elements its geometry carries (Slot values, ascending).
	struct Material
	{
		uint32_t effect = 0;
		uint8_t bucket = 0;
		uint16_t block = 0; // the parameter block the instance reserves (at least the effect's; 0: 4096)
		std::vector<std::pair<uint32_t, std::string>> textures;
		std::vector<std::pair<uint32_t, std::vector<uint8_t>>> constants;
		std::vector<uint8_t> slots;

		void Texture(uint32_t parameter, const std::string& name);
	};

	struct Mesh
	{
		uint32_t material = 0;
		std::vector<Vertex> vertices; // at most 65535
		std::vector<uint32_t> indices; // a triangle list
	};

	struct Drawable
	{
		std::string name; // the file's, without the extension
		std::vector<Material> materials;
		std::array<std::vector<Mesh>, 4> lods; // high, medium, low, very low
		std::array<float, 4> lodDistances{9998, 9998, 9998, 9998};
	};

	// The shader `index` of a game drawable (at `at`), with the vertex elements of the first geometry drawn with it.
	bool ReadMaterial(const ue5export::drawable::View& view, uint64_t at, uint32_t index, Material& out, std::string& error);

	bool Write(const Drawable& drawable, loader::convert::Bytes& out, std::string& error);

	// The box of the highest level of detail (for the archetype).
	void Box(const Drawable& drawable, std::array<float, 3>& min, std::array<float, 3>& max);
}
