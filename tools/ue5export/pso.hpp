#pragma once
#include <cstdint>
#include <map>
#include <string>
#include <vector>

#include "convert/resource.hpp"

// The game's other metadata format, PSO (research/phase0.md §59.2): big-endian sections "PSIN" (the data), "PMAP"
// (its blocks: structure, offset, length; the root's number), "PSCH" (the structures: fields as name hash, type,
// offset, reference; enums), then signatures and strings. Names are case-sensitive joaat hashes (as in meta files);
// a pointer is the block number (1-based) | offset << 12; an array is {pointer u32, pad u32, count u16, capacity u16,
// pad u32}. Used by the archives' _manifest.ymf (map data groups, HD texture bindings).
namespace ue5export::pso
{
	enum Type : uint8_t
	{
		kU32 = 0x06,
		kString = 0x0b, // subtype 0: inline char[ref >> 16]; 2: a pointer to a C string; 7: a name hash
		kStruct = 0x0c,
		kArray = 0x0d, // ref: the entry describing its elements
		kEnum = 0x0e,
		kFlags = 0x0f,
	};

	struct Field
	{
		uint32_t name = 0;
		uint8_t type = 0, subtype = 0;
		uint16_t offset = 0;
		uint32_t ref = 0;
	};

	struct Structure
	{
		uint32_t name = 0, size = 0;
		std::vector<Field> fields;
		const Field* Find(uint32_t fieldName) const;
	};

	struct Ref
	{
		const class Pso* pso = nullptr;
		const Structure* type = nullptr;
		size_t at = 0; // in the file
		explicit operator bool() const { return pso && type; }

		uint32_t U32(uint32_t field) const;
		std::string String(uint32_t field) const;
		std::vector<uint32_t> U32Array(uint32_t field) const; // arrays of u32 / name hashes
		std::vector<Ref> Array(uint32_t field) const;         // arrays of structures
	};

	class Pso
	{
	public:
		bool Read(const loader::convert::Bytes& file, std::string& error);
		Ref Root() const;
		uint32_t U32(size_t at) const;
		const uint8_t* At(size_t at, size_t size) const { return at + size <= f_.size() ? f_.data() + at : nullptr; }
		const Structure* Find(uint32_t name) const;
		// A pointer -> its offset in the file and the block's structure.
		bool Resolve(uint32_t pointer, size_t& at, uint32_t& structure) const;

	private:
		struct Block
		{
			uint32_t structure = 0, offset = 0, length = 0;
		};
		loader::convert::Bytes f_;
		size_t data_ = 0; // PSIN's start
		std::vector<Block> blocks_;
		uint32_t root_ = 0;
		std::map<uint32_t, Structure> structures_;
	};
}
