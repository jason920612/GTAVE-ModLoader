#pragma once
#include <array>
#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

#include "../ue5export/gamefiles.hpp"

// The asset catalog (research/phase0.md §61.14): every archetype of the game's map with what the world builder needs to
// place it elsewhere: its size, texture dictionary, physics, how often and at which level of detail the game places it,
// and its LOD (the archetype of the entity its HD entities hang under). Built from the player's own game files; kept as
// a tab-separated file outside the repository.
namespace worldgen::catalog
{
	enum class Kind : uint8_t
	{
		Other,
		Building, // a big static model placed at HD level (its collision is in the area's collision files, not its own)
		Prop,     // a prop (own collision or a fragment)
		Tree,     // trees, palms, bushes
		Lod,      // a LOD model (lodLevel 1)
		Slod,     // a lower one (SLOD1-4)
		Interior, // an MLO
	};
	const char* Name(Kind kind);

	struct Item
	{
		std::string name;    // as the model file is named ("" when there is no such file)
		uint32_t hash = 0;
		Kind kind = Kind::Other;
		std::string area;    // the name's first two parts ("dt1_05")
		std::string ytyp;
		uint32_t txd = 0, physics = 0, flags = 0, drawableDictionary = 0;
		uint8_t assetType = 0;
		bool fragment = false; // its model is a .yft
		std::array<float, 3> min{}, max{};
		float lodDist = 0, hdTextureDist = 0;
		uint32_t uses = 0;   // entities in the game's maps
		uint32_t lod = 0;    // the archetype its HD entities' LOD parent mostly is (0: none)
		std::string lodName;
		float entityLodDist = 0; // the median lodDist the game's entities give it
		float upright = 1;       // the share of its entities that stand upright (turned about z alone; parts of
		                         // buildings - roofs, frames - are often set tilted)
	};

	bool Build(const ue5export::GameFiles& files, std::vector<Item>& out, std::string& error);
	bool Save(const std::vector<Item>& items, const std::filesystem::path& file, std::string& error);
	bool Load(const std::filesystem::path& file, std::vector<Item>& out, std::string& error);
}
