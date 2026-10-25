#pragma once
#include <array>
#include <cstdint>
#include <string>
#include <vector>

#include "../ue5export/gamefiles.hpp"
#include "convert/resource.hpp"

// Map files (.ymap: entities) and archetype files (.ytyp: what models they are), written with the meta writer from
// templates among the game's own files (research/phase0.md §61.9, §61.12).
namespace worldgen::mapfiles
{
	// A model of our own: a drawable (assetType 2) with the same name, textures from `textureDictionary` (a game
	// dictionary: its parents are searched too).
	struct Archetype
	{
		std::string name;
		uint32_t assetName = 0; // the model's name (0: the archetype's own); a game model under a new name: an alias
		uint32_t textureDictionary = 0;
		uint32_t physicsDictionary = 0;
		uint32_t flags = 0;
		float lodDist = 500, hdTextureDist = 250;
		std::array<float, 3> min{}, max{};
	};

	// An entity: rotation as the entity is turned (the file stores its inverse), lodLevel 5 = an HD entity with no
	// LOD parent.
	struct Entity
	{
		uint32_t archetype = 0;
		std::array<float, 3> position{};
		std::array<float, 4> rotation{0, 0, 0, 1}; // x, y, z, w
		float scaleXY = 1, scaleZ = 1, lodDist = 500;
		uint32_t flags = 0, lodLevel = 5;
		int32_t parentIndex = -1;
		float radius = 0; // how far it reaches from its position (for the map's boxes)
	};

	// One of the game's archetypes by name (every archetype file is searched).
	bool FindArchetype(const ue5export::GameFiles& files, const std::string& name, Archetype& out, std::string& error);

	bool WriteTypes(const ue5export::GameFiles& files, const std::string& name, const std::vector<Archetype>& archetypes, loader::convert::Bytes& out,
	    std::string& error);
	bool WriteMap(const ue5export::GameFiles& files, const std::string& name, const std::vector<Entity>& entities, loader::convert::Bytes& out,
	    std::string& error);
}
