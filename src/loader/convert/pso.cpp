#include "pso.hpp"

#include <algorithm>
#include <cstring>
#include <format>
#include <tuple>

namespace loader::convert::pso
{
	namespace
	{
		constexpr uint8_t kStruct = 0x0c, kArray = 0x0d, kString = 0x0b;
		constexpr char kPad = 0x70; // ('p': what the game's files fill gaps with)

		uint32_t U32(const std::string& f, size_t at)
		{
			if (at + 4 > f.size())
				return 0;
			const auto* p = reinterpret_cast<const uint8_t*>(f.data() + at);
			return uint32_t(p[0]) << 24 | uint32_t(p[1]) << 16 | uint32_t(p[2]) << 8 | p[3];
		}
		uint16_t U16(const std::string& f, size_t at)
		{
			if (at + 2 > f.size())
				return 0;
			const auto* p = reinterpret_cast<const uint8_t*>(f.data() + at);
			return static_cast<uint16_t>(p[0] << 8 | p[1]);
		}
		void Put32(std::string& f, size_t at, uint32_t v)
		{
			f[at] = static_cast<char>(v >> 24), f[at + 1] = static_cast<char>(v >> 16), f[at + 2] = static_cast<char>(v >> 8), f[at + 3] = static_cast<char>(v);
		}
		void Put16(std::string& f, size_t at, uint16_t v)
		{
			f[at] = static_cast<char>(v >> 8), f[at + 1] = static_cast<char>(v);
		}
		uint32_t Tag(const char* t) { return uint32_t(uint8_t(t[0])) << 24 | uint32_t(uint8_t(t[1])) << 16 | uint32_t(uint8_t(t[2])) << 8 | uint8_t(t[3]); }
	}

	bool Entries::Read(const std::string& file, std::string& error)
	{
		*this = {};
		file_ = file;
		size_t psch = 0;
		for (size_t at = 0; at + 8 <= file_.size();)
		{
			const uint32_t tag = U32(file_, at), size = U32(file_, at + 4);
			if (size < 8)
				break;
			if (tag == Tag("PSIN"))
				psin_ = at + 1; // (+1: found; the section starts at 0 in the game's files)
			else if (tag == Tag("PMAP"))
				pmap_ = at;
			else if (tag == Tag("PSCH"))
				psch = at;
			at += size;
		}
		if (!psin_ || !pmap_ || !psch)
			return error = "not a PSO file", false;
		--psin_;
		root_ = U32(file_, pmap_ + 8);
		const uint16_t blocks = U16(file_, pmap_ + 12);
		for (uint16_t i = 0; i < blocks; ++i)
		{
			blockStructure_.push_back(U32(file_, pmap_ + 16 + 16 * i));
			blockOffset_.push_back(U32(file_, pmap_ + 20 + 16 * i));
			blockLength_.push_back(U32(file_, pmap_ + 28 + 16 * i));
		}
		if (!root_ || root_ > blocks)
			return error = "no root block", false;
		// the structures: (name, offset) per definition; a structure's fields (name, type << 24 | subtype << 16 | offset,
		// reference)
		struct Field
		{
			uint32_t name;
			uint8_t type, subtype;
			uint16_t offset;
			uint32_t ref;
		};
		const auto fieldsOf = [&](uint32_t structure, uint32_t& size) {
			std::vector<Field> fields;
			const uint32_t count = U32(file_, psch + 8);
			for (uint32_t i = 0; i < count; ++i)
			{
				if (U32(file_, psch + 12 + 8 * i) != structure)
					continue;
				const size_t d = psch + U32(file_, psch + 16 + 8 * i);
				const uint32_t head = U32(file_, d);
				if (head >> 24 != 0)
					break; // an enum
				size = U32(file_, d + 4);
				for (uint32_t k = 0; k < (head & 0xFFFF); ++k)
				{
					const size_t e = d + 12 + 12 * k;
					const uint32_t info = U32(file_, e + 4);
					fields.push_back({U32(file_, e), uint8_t(info >> 24), uint8_t(info >> 16), uint16_t(info), U32(file_, e + 8)});
				}
				break;
			}
			return fields;
		};
		uint32_t rootSize = 0;
		const auto rootFields = fieldsOf(blockStructure_[root_ - 1], rootSize);
		const Field* array = nullptr;
		for (const Field& f : rootFields)
			if (f.type == kArray && f.name != 0x100 && f.ref < rootFields.size() && rootFields[f.ref].type == kStruct)
			{
				array = &f;
				elementStructure_ = rootFields[f.ref].ref;
				break;
			}
		if (!array)
			return error = "the root has no array of structures", false;
		arrayOffset_ = array->offset;
		for (const Field& f : fieldsOf(elementStructure_, elementSize_))
		{
			if (f.type == kString && f.subtype == 2)
				nameOffsets_.push_back(f.offset);
			else if (f.type == kArray || (f.type == kString && f.subtype != 7 && f.subtype != 0))
				return error = std::format("an entry field ({:08x}) points elsewhere: not supported", f.name), false;
		}
		if (!elementSize_ || nameOffsets_.empty())
			return error = "the entries have no name", false;
		const auto resolve = [&](uint32_t pointer, size_t& at) {
			const uint32_t block = pointer & 0xFFF, offset = pointer >> 12;
			if (!block || block > blockOffset_.size() || offset >= blockLength_[block - 1])
				return false;
			at = psin_ + blockOffset_[block - 1] + offset;
			return true;
		};
		const size_t rootAt = psin_ + blockOffset_[root_ - 1];
		size_t data = 0;
		const uint16_t count = U16(file_, rootAt + arrayOffset_ + 8);
		if (count && !resolve(U32(file_, rootAt + arrayOffset_), data))
			return error = "the array is not in the file", false;
		for (uint16_t i = 0; i < count; ++i)
		{
			Entry e;
			e.bytes = file_.substr(data + static_cast<size_t>(i) * elementSize_, elementSize_);
			if (e.bytes.size() != elementSize_)
				return error = "the array runs past the file", false;
			size_t name = 0;
			if (resolve(U32(e.bytes, nameOffsets_[0]), name))
				e.key = std::string(file_.c_str() + name);
			entries_.push_back(std::move(e));
		}
		return true;
	}

	int Entries::Find(const std::string& key) const
	{
		for (size_t i = 0; i < entries_.size(); ++i)
			if (_stricmp(entries_[i].key.c_str(), key.c_str()) == 0)
				return static_cast<int>(i);
		return -1;
	}

	int Entries::Put(const Entries& from, size_t index, const std::string& key)
	{
		if (from.elementStructure_ != elementStructure_ || from.elementSize_ != elementSize_ || from.nameOffsets_ != nameOffsets_ || index >= from.entries_.size())
			return -1;
		Entry e = from.entries_[index];
		// (its other names, read from its own file, are written as ours too: the pointers are rewritten)
		e.own = true;
		e.key = key;
		const int at = Find(key);
		if (at >= 0)
		{
			entries_[at] = std::move(e);
			return 0;
		}
		entries_.push_back(std::move(e));
		return 1;
	}

	bool Entries::Write(std::string& out, std::string& error) const
	{
		const size_t psinSize = U32(file_, psin_ + 4);
		std::string psin = file_.substr(psin_, psinSize);
		const auto align = [&] {
			while (psin.size() % 16)
				psin += kPad;
		};
		// the new array, then the names of our entries
		align();
		const uint32_t arrayOffset = static_cast<uint32_t>(psin.size());
		const uint32_t arrayBlock = static_cast<uint32_t>(blockOffset_.size()) + 1, namesBlock = arrayBlock + 1;
		std::string names;
		for (const Entry& e : entries_)
		{
			std::string bytes = e.bytes;
			if (e.own)
				for (size_t k = 0; k < nameOffsets_.size(); ++k)
				{
					// (the key in the first name field; the others as the source file had them... which only the source can
					// read: an entry copied from another file keeps its key alone, the rest empty)
					const std::string text = k == 0 ? e.key : std::string();
					Put32(bytes, nameOffsets_[k], namesBlock | static_cast<uint32_t>(names.size()) << 12);
					names += text;
					names += '\0';
				}
			psin += bytes;
		}
		const uint32_t arrayLength = static_cast<uint32_t>(psin.size() - arrayOffset);
		align();
		const uint32_t namesOffset = static_cast<uint32_t>(psin.size());
		psin += names;
		if (names.empty())
			psin += '\0';
		const uint32_t namesLength = static_cast<uint32_t>(psin.size() - namesOffset);
		align();
		if (entries_.size() > 0xFFFF || arrayLength >= (1u << 20) || namesLength >= (1u << 20))
			return error = "too many entries", false;
		Put32(psin, 4, static_cast<uint32_t>(psin.size()));
		// the root's array field: the new array
		const size_t field = blockOffset_[root_ - 1] + arrayOffset_;
		Put32(psin, field, arrayBlock);
		Put16(psin, field + 8, static_cast<uint16_t>(entries_.size()));
		Put16(psin, field + 10, static_cast<uint16_t>(entries_.size()));
		// the block table with the two new blocks (the names: the kind of block the file's own names are in)
		uint32_t charStructure = 1;
		for (size_t b = 0; b < blockStructure_.size(); ++b)
			if (blockStructure_[b] < 0x100)
			{
				charStructure = blockStructure_[b];
				break;
			}
		std::string pmap = file_.substr(pmap_, 16);
		const uint16_t blocks = static_cast<uint16_t>(blockOffset_.size() + 2);
		Put32(pmap, 4, 16 + 16u * blocks);
		Put16(pmap, 12, blocks);
		pmap += file_.substr(pmap_ + 16, 16 * blockOffset_.size());
		for (const auto& [structure, offset, length] : {std::tuple{elementStructure_, arrayOffset, arrayLength}, std::tuple{charStructure, namesOffset, namesLength}})
		{
			std::string b(16, '\0');
			Put32(b, 0, structure);
			Put32(b, 4, offset);
			Put32(b, 12, length);
			pmap += b;
		}
		out = psin + pmap;
		size_t chks = std::string::npos;
		for (size_t at = 0; at + 8 <= file_.size();)
		{
			const uint32_t tag = U32(file_, at), size = U32(file_, at + 4);
			if (size < 8)
				break;
			if (tag != Tag("PSIN") && tag != Tag("PMAP"))
			{
				if (tag == Tag("CHKS"))
					chks = out.size();
				out += file_.substr(at, size);
			}
			at += size;
		}
		if (chks != std::string::npos)
			Put32(out, chks + 8, static_cast<uint32_t>(out.size()));
		return true;
	}
}
