#include "meta.hpp"

#include <cstring>
#include <format>

#include "pages.hpp"

namespace worldgen::meta
{
	using loader::convert::Bytes;
	using loader::convert::Get;
	using loader::convert::Put;

	namespace
	{
		constexpr uint64_t kVtable = 0x14069B018; // what the game's own files carry there
		constexpr uint32_t kMagic = 0x50524430, kRoot = 0x50;

		bool At(const Bytes& v, uint64_t pointer, size_t size, uint32_t& out)
		{
			if ((pointer >> 28) != 5 || (pointer >> 32) != 0 || (pointer & 0x0FFFFFFF) + size > v.size())
				return false;
			out = static_cast<uint32_t>(pointer & 0x0FFFFFFF);
			return true;
		}

		uint64_t Ptr(uint32_t offset)
		{
			return static_cast<uint64_t>(loader::convert::kVirtual) + offset;
		}
	}

	uint32_t Hash(const std::string& text)
	{
		uint32_t h = 0;
		for (const char c : text)
		{
			h += static_cast<uint8_t>(c);
			h += h << 10;
			h ^= h >> 6;
		}
		h += h << 3;
		h ^= h >> 11;
		return h + (h << 15);
	}

	bool Writer::Template(const Bytes& file, std::string& error)
	{
		loader::convert::Resource r;
		if (!loader::convert::Read(file, r, error))
			return false;
		const Bytes& v = r.virtualBlock;
		uint32_t structs = 0, enums = 0;
		const uint16_t structCount = Get<uint16_t>(v, 0x48), enumCount = Get<uint16_t>(v, 0x4A);
		if (!At(v, Get<uint64_t>(v, 0x20), structCount * 0x20ull, structs) || (enumCount && !At(v, Get<uint64_t>(v, 0x28), enumCount * 0x18ull, enums)))
			return error = "not a metadata resource", false;
		structures_.clear();
		enums_.clear();
		for (uint16_t i = 0; i < structCount; ++i)
		{
			Structure s;
			std::memcpy(s.raw.data(), v.data() + structs + 0x20 * i, 0x20);
			const uint16_t count = Get<uint16_t>(v, structs + 0x20 * i + 0x1E);
			uint32_t entries = 0;
			if (count && !At(v, Get<uint64_t>(v, structs + 0x20 * i + 0x10), count * 16ull, entries))
				return error = "structure entries outside the block", false;
			s.entries.resize(count);
			for (uint16_t k = 0; k < count; ++k)
				std::memcpy(s.entries[k].data(), v.data() + entries + 16 * k, 16);
			structures_.push_back(std::move(s));
		}
		for (uint16_t i = 0; i < enumCount; ++i)
		{
			Enum e;
			std::memcpy(e.raw.data(), v.data() + enums + 0x18 * i, 0x18);
			const uint32_t count = Get<uint32_t>(v, enums + 0x18 * i + 0x10);
			uint32_t values = 0;
			if (count && !At(v, Get<uint64_t>(v, enums + 0x18 * i + 0x08), count * 8ull, values))
				return error = "enum values outside the block", false;
			e.values.resize(count);
			for (uint32_t k = 0; k < count; ++k)
				std::memcpy(e.values[k].data(), v.data() + values + 8 * k, 8);
			enums_.push_back(std::move(e));
		}
		return true;
	}

	bool Writer::HasStructure(uint32_t name) const
	{
		for (const Structure& s : structures_)
		{
			uint32_t n;
			std::memcpy(&n, s.raw.data(), 4);
			if (n == name)
				return true;
		}
		return false;
	}

	int Writer::Offset(uint32_t structure, uint32_t field) const
	{
		for (const Structure& s : structures_)
		{
			uint32_t n;
			std::memcpy(&n, s.raw.data(), 4);
			if (n != structure)
				continue;
			for (const auto& e : s.entries)
			{
				uint32_t name;
				int32_t offset;
				std::memcpy(&name, e.data(), 4);
				std::memcpy(&offset, e.data() + 4, 4);
				if (name == field)
					return offset;
			}
		}
		return -1;
	}

	uint32_t Writer::Size(uint32_t structure) const
	{
		for (const Structure& s : structures_)
		{
			uint32_t n, size;
			std::memcpy(&n, s.raw.data(), 4);
			std::memcpy(&size, s.raw.data() + 0x18, 4);
			if (n == structure)
				return size;
		}
		return 0;
	}

	uint32_t Writer::AddBlock(uint32_t structure, Bytes data)
	{
		blocks_.push_back({structure, std::move(data)});
		return static_cast<uint32_t>(blocks_.size());
	}

	bool Writer::Write(uint32_t rootBlock, Bytes& out, std::string& error) const
	{
		if (!rootBlock || rootBlock > blocks_.size() || structures_.empty())
			return error = "no root block or no definitions", false;
		Pages pages;
		const uint32_t root = pages.Alloc(kRoot);
		const uint32_t structs = pages.Alloc(static_cast<uint32_t>(structures_.size() * 0x20));
		std::vector<uint32_t> entries(structures_.size());
		for (size_t i = 0; i < structures_.size(); ++i)
			entries[i] = structures_[i].entries.empty() ? 0 : pages.Alloc(static_cast<uint32_t>(structures_[i].entries.size() * 16));
		const uint32_t enums = enums_.empty() ? 0 : pages.Alloc(static_cast<uint32_t>(enums_.size() * 0x18));
		std::vector<uint32_t> values(enums_.size());
		for (size_t i = 0; i < enums_.size(); ++i)
			values[i] = enums_[i].values.empty() ? 0 : pages.Alloc(static_cast<uint32_t>(enums_[i].values.size() * 8));
		const uint32_t blockList = pages.Alloc(static_cast<uint32_t>(blocks_.size() * 0x10));
		std::vector<uint32_t> data(blocks_.size());
		for (size_t i = 0; i < blocks_.size(); ++i)
		{
			if (blocks_[i].data.size() > Pages::kSize)
				return error = std::format("block {} is {} bytes (a page holds {})", i + 1, blocks_[i].data.size(), Pages::kSize), false;
			data[i] = pages.Alloc(static_cast<uint32_t>(std::max<size_t>(blocks_[i].data.size(), 16)));
		}
		const uint32_t pageMap = pages.PageMap();
		if (pages.used.size() > 127)
			return error = "too large (more than 127 pages of 16 KB)", false;

		loader::convert::Resource r;
		r.version = 2;
		r.virtualFlags = 1u | static_cast<uint32_t>(pages.used.size()) << 17;
		r.physicalFlags = 0x20000000;
		Bytes& v = r.virtualBlock;
		v.assign(pages.used.size() * Pages::kSize, 0);
		Put<uint64_t>(v, root + 0x00, kVtable);
		Put<uint64_t>(v, root + 0x08, Ptr(pageMap));
		Put<uint32_t>(v, root + 0x10, kMagic);
		v[root + 0x14] = 0x79;
		Put<uint16_t>(v, root + 0x16, 1);
		Put<uint32_t>(v, root + 0x1C, rootBlock);
		Put<uint64_t>(v, root + 0x20, Ptr(structs));
		if (enums)
			Put<uint64_t>(v, root + 0x28, Ptr(enums));
		Put<uint64_t>(v, root + 0x30, Ptr(blockList));
		Put<uint16_t>(v, root + 0x48, static_cast<uint16_t>(structures_.size()));
		Put<uint16_t>(v, root + 0x4A, static_cast<uint16_t>(enums_.size()));
		Put<uint16_t>(v, root + 0x4C, static_cast<uint16_t>(blocks_.size()));
		for (size_t i = 0; i < structures_.size(); ++i)
		{
			const uint32_t at = structs + 0x20 * static_cast<uint32_t>(i);
			std::memcpy(v.data() + at, structures_[i].raw.data(), 0x20);
			Put<uint64_t>(v, at + 0x10, entries[i] ? Ptr(entries[i]) : 0);
			for (size_t k = 0; k < structures_[i].entries.size(); ++k)
				std::memcpy(v.data() + entries[i] + 16 * k, structures_[i].entries[k].data(), 16);
		}
		for (size_t i = 0; i < enums_.size(); ++i)
		{
			const uint32_t at = enums + 0x18 * static_cast<uint32_t>(i);
			std::memcpy(v.data() + at, enums_[i].raw.data(), 0x18);
			Put<uint64_t>(v, at + 0x08, values[i] ? Ptr(values[i]) : 0);
			for (size_t k = 0; k < enums_[i].values.size(); ++k)
				std::memcpy(v.data() + values[i] + 8 * k, enums_[i].values[k].data(), 8);
		}
		for (size_t i = 0; i < blocks_.size(); ++i)
		{
			const uint32_t at = blockList + 0x10 * static_cast<uint32_t>(i);
			Put<uint32_t>(v, at, blocks_[i].structure);
			Put<uint32_t>(v, at + 4, static_cast<uint32_t>(blocks_[i].data.size()));
			Put<uint64_t>(v, at + 8, Ptr(data[i]));
			if (!blocks_[i].data.empty())
				std::memcpy(v.data() + data[i], blocks_[i].data.data(), blocks_[i].data.size());
		}
		v[pageMap + 8] = static_cast<uint8_t>(pages.used.size());
		return loader::convert::Write(r, out, error);
	}
}
