#pragma once
#include <cstdint>
#include <filesystem>
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
		// A texture of a dictionary by their names.
		bool Find(const std::string& dictionary, const std::string& texture, Texture& out) { return In(Joaat(dictionary), Lower(texture), out); }

		// Every texture dictionary's texture names (lower name -> the dictionary's name hash): where a texture that no
		// dictionary of its model's chain has is found after all (the game finds them too). Shared by all resolvers.
		using Index = std::map<std::string, uint32_t>;
		static void SetIndex(std::shared_ptr<const Index> index);
		// Builds the index from every .ytd (workers threads), or loads it from `cache` (written after building).
		static std::shared_ptr<const Index> BuildIndex(const GameFiles& files, const std::filesystem::path& gameDir,
		    const loader::convert::Decryptor& decrypt, unsigned workers, const std::filesystem::path& cache);

	private:
		static std::shared_ptr<const Index> s_index;
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
