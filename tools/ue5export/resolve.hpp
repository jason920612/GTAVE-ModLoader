#pragma once
#include <cstdint>
#include <list>
#include <map>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

#include "drawable.hpp"
#include "gamefiles.hpp"
#include "mapdata.hpp"

// Models and their textures as the game finds them (research/phase0.md §58.4): the model file of an archetype, and
// for each shader texture the drawable's HD dictionary, its embedded textures, the archetype's dictionary (HD version
// first) and its gtxd parents, then the map's shared detail textures.
namespace ue5export
{
	std::string Lower(std::string s);
	// joaat of a lower-cased name: how the game hashes file, archetype and shader parameter names.
	uint32_t Joaat(std::string_view s);
	// A shader parameter's name by its hash (the ones the export knows), else the hash in hex.
	std::string ParameterName(uint32_t hash);

	class Resolver
	{
	public:
		using Dictionary = std::map<std::string, drawable::Texture>; // by lower name

		struct Texture
		{
			std::shared_ptr<const Dictionary> dictionary; // keeps the texture alive
			const drawable::Texture* texture = nullptr;
			std::string where; // the dictionary's name ("dt1_12_build+hi")
			std::string key;   // the texture's lower name
		};

		struct Model
		{
			drawable::Model model;
			std::string file;                                 // where it came from
			std::vector<std::map<uint32_t, Texture>> textures; // per material: parameter hash -> texture
			std::vector<std::string> missing;                  // textures not found
		};

		explicit Resolver(const GameFiles& files, size_t cacheBytes = size_t{2} << 30);

		// A file's name without extension by its name hash (models, dictionaries), else the hash in hex.
		std::string Stem(uint32_t hash) const;
		// The model an archetype draws, with its textures.
		bool Load(const mapdata::Archetype& archetype, Model& out, std::string& error);

	private:
		const GameFiles& files_;
		std::map<std::string, std::map<uint32_t, std::string>> byHash_; // extension -> name hash -> file
		std::map<uint32_t, uint32_t> parents_;                          // dictionary -> parent dictionary
		// Dictionaries read so far, least recently used first, up to cacheBytes_ of texture data.
		std::map<uint32_t, std::pair<std::shared_ptr<const Dictionary>, std::list<uint32_t>::iterator>> cache_;
		std::list<uint32_t> recent_;
		size_t cacheBytes_, cached_ = 0;

		std::shared_ptr<const Dictionary> Get(uint32_t hash);
		std::string DictionaryName(uint32_t hash) const;
		bool In(uint32_t hash, const std::string& key, Texture& out);
		bool Chain(uint32_t hash, const std::string& key, Texture& out);
		void ReadParents();
	};
}
