#pragma once
#include <array>
#include <cstdint>
#include <string>
#include <vector>

#include "convert/resource.hpp"

// Writing the game's self-describing metadata resources (ymap, ytyp; research/phase0.md §58.2, §61.9). The structure
// and enum definitions are taken from one of the game's own files of the kind (they describe the game's types, so they
// are the same in every such file); the data is ours, as blocks of one structure each (at most a 16 KB page each), and
// a pointer inside the data is (block number, 1-based) | (offset << 12).
//   root (0x50): +0x10 'PRD0' magic, +0x14 0x79 and 1, +0x1C the root block, +0x20 / +0x28 / +0x30 the structures,
//   enums, blocks, +0x40 0 (the game's own files carry more there; the game's runtime metas have 0), +0x48 their counts.
//   A structure (0x20): name, key, +0x08 8 bytes, +0x10 its entries (16 bytes each), +0x18 size, +0x1E entry count.
//   An enum (0x18): name, key, +0x08 its values (name hash, value), +0x10 count. A block (0x10): structure, size, data.
namespace worldgen::meta
{
	class Writer
	{
	public:
		// The definitions of a game file of the kind (every structure and enum it has).
		bool Template(const loader::convert::Bytes& file, std::string& error);
		bool HasStructure(uint32_t name) const;
		// A field's offset in a structure (by the name hashes), or -1.
		int Offset(uint32_t structure, uint32_t field) const;
		uint32_t Size(uint32_t structure) const;

		// A block of one structure (or of a basic type: 7 pointers, 0x4A hashes...) holding `data` (at most 16 KB);
		// returns its number (1-based).
		uint32_t AddBlock(uint32_t structure, loader::convert::Bytes data);
		loader::convert::Bytes& Block(uint32_t number) { return blocks_[number - 1].data; }
		static uint64_t Pointer(uint32_t block, uint32_t offset) { return block | static_cast<uint64_t>(offset) << 12; }

		bool Write(uint32_t rootBlock, loader::convert::Bytes& out, std::string& error) const;

	private:
		struct Structure
		{
			std::array<uint8_t, 0x20> raw{};
			std::vector<std::array<uint8_t, 16>> entries;
		};
		struct Enum
		{
			std::array<uint8_t, 0x18> raw{};
			std::vector<std::array<uint8_t, 8>> values;
		};
		struct DataBlock
		{
			uint32_t structure = 0;
			loader::convert::Bytes data;
		};
		std::vector<Structure> structures_;
		std::vector<Enum> enums_;
		std::vector<DataBlock> blocks_;
	};

	// The hashes of structure and field names (one-at-a-time, case as written: "archetypeName"). Model and other asset
	// names are hashed over the lower-case name (loader::convert::Joaat).
	uint32_t Hash(const std::string& text);
}
