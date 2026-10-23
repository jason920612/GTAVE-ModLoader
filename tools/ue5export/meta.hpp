#pragma once
#include <array>
#include <cstdint>
#include <cstring>
#include <map>
#include <optional>
#include <string>
#include <vector>

#include "convert/resource.hpp"

// The game's self-describing metadata resources (ymap, ytyp; research/phase0.md §58.2): the file carries its own
// structure definitions (names as hashes of the field names), and the data sits in blocks of one structure each.
// Pointers inside the data are (block number, 1-based) | (offset << 12).
namespace ue5export::meta
{
	struct Field
	{
		uint32_t name = 0;
		int32_t offset = 0;
		uint8_t type = 0;
	};

	struct Structure
	{
		uint32_t name = 0;
		uint32_t size = 0;
		std::vector<Field> fields;
		const Field* Find(uint32_t fieldName) const;
	};

	struct Block
	{
		uint32_t structure = 0; // a structure's name hash, or a basic type number (pointer arrays: 7)
		uint32_t size = 0;
		size_t at = 0;          // in the virtual block
	};

	// A structure instance inside the resource.
	struct Ref
	{
		const class Meta* meta = nullptr;
		const Structure* type = nullptr;
		size_t at = 0;
		explicit operator bool() const { return meta && type; }

		template <typename T>
		T Get(uint32_t field) const;
		// The elements of an array field: structures (inline arrays) or the structures their pointers point at.
		std::vector<Ref> Array(uint32_t field) const;
		// An inline structure field.
		Ref Struct(uint32_t field, uint32_t structure) const;
		// The elements of an array of basic values: 4-byte ones (u32, hashes), or vectors (16 bytes each).
		std::vector<uint32_t> U32Array(uint32_t field) const;
		std::vector<std::array<float, 4>> VectorArray(uint32_t field) const;
	};

	class Meta
	{
	public:
		bool Read(const loader::convert::Bytes& file, std::string& error);
		Ref Root() const;
		const Structure* Find(uint32_t name) const;
		std::optional<size_t> Resolve(uint64_t pointer) const; // a data pointer -> offset in the virtual block
		const uint8_t* At(size_t at) const { return v_.data() + at; }
		size_t Size() const { return v_.size(); }
		uint32_t BlockStructure(uint64_t pointer) const;
		const std::map<uint32_t, Structure>& Structures() const { return structures_; }
		const std::vector<Block>& Blocks() const { return blocks_; }

	private:
		loader::convert::Bytes v_;
		std::map<uint32_t, Structure> structures_;
		std::vector<Block> blocks_;
		uint32_t rootBlock_ = 0;
	};

	template <typename T>
	T Ref::Get(uint32_t field) const
	{
		T value{};
		if (const Field* f = type ? type->Find(field) : nullptr; f && at + f->offset + sizeof(T) <= meta->Size())
			std::memcpy(&value, meta->At(at + f->offset), sizeof(T));
		return value;
	}

	// joaat of a field or structure name, as the game hashes them (case kept).
	constexpr uint32_t Hash(std::string_view name)
	{
		uint32_t h = 0;
		for (const char c : name)
		{
			h += static_cast<uint8_t>(c);
			h += h << 10;
			h ^= h >> 6;
		}
		h += h << 3;
		h ^= h >> 11;
		return h + (h << 15);
	}
}
