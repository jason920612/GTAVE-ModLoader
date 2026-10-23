#include "meta.hpp"

#include <format>

namespace ue5export::meta
{
	namespace
	{
		constexpr uint8_t kStruct = 0x05, kPointer = 0x07, kArray = 0x52;
		constexpr uint32_t kArrayInfo = 0x100; // the field before an array: its element type

		template <typename T>
		T Peek(const loader::convert::Bytes& v, size_t at)
		{
			T value{};
			if (at + sizeof(T) <= v.size())
				std::memcpy(&value, v.data() + at, sizeof(T));
			return value;
		}

		std::optional<size_t> Virtual(uint64_t p, size_t size)
		{
			if ((p >> 28) != 5 || (p & 0x0FFFFFFF) >= size)
				return std::nullopt;
			return static_cast<size_t>(p & 0x0FFFFFFF);
		}

		// A data block's place: the virtual segment, or the physical one (big files put most of their data blocks
		// there), which follows the virtual one in v_.
		std::optional<size_t> Segment(uint64_t p, size_t virtualSize, size_t size)
		{
			const size_t offset = static_cast<size_t>(p & 0x0FFFFFFF);
			if ((p >> 32) == 0 && (p >> 28) == 5 && offset < virtualSize)
				return offset;
			if ((p >> 32) == 0 && (p >> 28) == 6 && virtualSize + offset < size)
				return virtualSize + offset;
			return std::nullopt;
		}
	}

	const Field* Structure::Find(uint32_t fieldName) const
	{
		for (const Field& f : fields)
			if (f.name == fieldName)
				return &f;
		return nullptr;
	}

	bool Meta::Read(const loader::convert::Bytes& file, std::string& error)
	{
		loader::convert::Resource resource;
		if (!loader::convert::Read(file, resource, error))
			return false;
		v_ = std::move(resource.virtualBlock);
		const size_t virtualSize = v_.size();
		v_.insert(v_.end(), resource.physicalBlock.begin(), resource.physicalBlock.end());
		const auto structs = Virtual(Peek<uint64_t>(v_, 0x20), v_.size());
		const auto blocks = Virtual(Peek<uint64_t>(v_, 0x30), v_.size());
		const uint16_t structCount = Peek<uint16_t>(v_, 0x48), blockCount = Peek<uint16_t>(v_, 0x4C);
		rootBlock_ = Peek<uint32_t>(v_, 0x1C);
		if (!structs || !blocks)
			return error = "not a metadata resource", false;
		for (uint16_t i = 0; i < structCount; ++i)
		{
			const size_t o = *structs + i * 0x20u;
			Structure s;
			s.name = Peek<uint32_t>(v_, o);
			s.size = Peek<uint32_t>(v_, o + 0x18);
			const auto entries = Virtual(Peek<uint64_t>(v_, o + 0x10), v_.size());
			const uint16_t count = Peek<uint16_t>(v_, o + 0x1E);
			for (uint16_t k = 0; entries && k < count; ++k)
			{
				const size_t e = *entries + k * 16u;
				s.fields.push_back({Peek<uint32_t>(v_, e), Peek<int32_t>(v_, e + 4), Peek<uint8_t>(v_, e + 8)});
			}
			structures_[s.name] = std::move(s);
		}
		for (uint16_t i = 0; i < blockCount; ++i)
		{
			const size_t o = *blocks + i * 0x10u;
			const auto at = Segment(Peek<uint64_t>(v_, o + 8), virtualSize, v_.size());
			// (a block that cannot be placed has no size: pointers into it resolve to nothing)
			blocks_.push_back({Peek<uint32_t>(v_, o), at ? Peek<uint32_t>(v_, o + 4) : 0, at.value_or(0)});
		}
		return true;
	}

	const Structure* Meta::Find(uint32_t name) const
	{
		const auto it = structures_.find(name);
		return it == structures_.end() ? nullptr : &it->second;
	}

	std::optional<size_t> Meta::Resolve(uint64_t pointer) const
	{
		const uint32_t block = static_cast<uint32_t>(pointer & 0xFFF), offset = static_cast<uint32_t>((pointer >> 12) & 0xFFFFF);
		if (!block || block > blocks_.size() || offset >= blocks_[block - 1].size)
			return std::nullopt;
		return blocks_[block - 1].at + offset;
	}

	uint32_t Meta::BlockStructure(uint64_t pointer) const
	{
		const uint32_t block = static_cast<uint32_t>(pointer & 0xFFF);
		return block && block <= blocks_.size() ? blocks_[block - 1].structure : 0;
	}

	Ref Meta::Root() const
	{
		if (!rootBlock_ || rootBlock_ > blocks_.size())
			return {};
		const Block& b = blocks_[rootBlock_ - 1];
		return {this, Find(b.structure), b.at};
	}

	std::vector<Ref> Ref::Array(uint32_t field) const
	{
		std::vector<Ref> out;
		if (!type)
			return out;
		const Field* f = type->Find(field);
		if (!f || f->type != kArray)
			return out;
		// The element type is the entry right before the array (named 0x100).
		const Field* info = nullptr;
		for (size_t i = 1; i < type->fields.size(); ++i)
			if (&type->fields[i] == f && type->fields[i - 1].name == kArrayInfo)
				info = &type->fields[i - 1];
		uint64_t pointer = 0;
		uint16_t count = 0;
		std::memcpy(&pointer, meta->At(at + f->offset), 8);
		std::memcpy(&count, meta->At(at + f->offset + 8), 2);
		const auto data = meta->Resolve(pointer);
		if (!data || !count)
			return out;
		if (info && info->type == kPointer)
		{
			for (uint16_t i = 0; i < count; ++i)
			{
				uint64_t p = 0;
				std::memcpy(&p, meta->At(*data + i * 8u), 8);
				if (const auto element = meta->Resolve(p))
					out.push_back({meta, meta->Find(meta->BlockStructure(p)), *element});
			}
			return out;
		}
		const Structure* element = meta->Find(meta->BlockStructure(pointer));
		if (!element)
			return out;
		for (uint16_t i = 0; i < count; ++i)
			out.push_back({meta, element, *data + i * static_cast<size_t>(element->size)});
		return out;
	}

	Ref Ref::Struct(uint32_t field, uint32_t structure) const
	{
		const Field* f = type ? type->Find(field) : nullptr;
		if (!f || f->type != kStruct)
			return {};
		return {meta, meta->Find(structure), at + f->offset};
	}
}

namespace ue5export::meta
{
	namespace
	{
		// An array field's data and count.
		bool ArrayData(const Ref& r, uint32_t field, size_t& data, uint16_t& count)
		{
			const Field* f = r.type ? r.type->Find(field) : nullptr;
			if (!f || f->type != 0x52)
				return false;
			uint64_t pointer = 0;
			std::memcpy(&pointer, r.meta->At(r.at + f->offset), 8);
			std::memcpy(&count, r.meta->At(r.at + f->offset + 8), 2);
			const auto d = r.meta->Resolve(pointer);
			if (!d || !count)
				return false;
			data = *d;
			return true;
		}
	}

	std::vector<uint32_t> Ref::U32Array(uint32_t field) const
	{
		std::vector<uint32_t> out;
		size_t data = 0;
		uint16_t count = 0;
		if (!ArrayData(*this, field, data, count) || data + count * 4u > meta->Size())
			return out;
		out.resize(count);
		std::memcpy(out.data(), meta->At(data), count * 4u);
		return out;
	}

	std::vector<std::array<float, 4>> Ref::VectorArray(uint32_t field) const
	{
		std::vector<std::array<float, 4>> out;
		size_t data = 0;
		uint16_t count = 0;
		if (!ArrayData(*this, field, data, count) || data + count * 16u > meta->Size())
			return out;
		out.resize(count);
		std::memcpy(out.data(), meta->At(data), count * 16u);
		return out;
	}
}
