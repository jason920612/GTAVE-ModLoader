#include "pso.hpp"

#include <cstring>

namespace ue5export::pso
{
	namespace
	{
		uint32_t Tag(const char* t) { return uint32_t(uint8_t(t[0])) << 24 | uint32_t(uint8_t(t[1])) << 16 | uint32_t(uint8_t(t[2])) << 8 | uint8_t(t[3]); }
	}

	const Field* Structure::Find(uint32_t fieldName) const
	{
		for (const Field& f : fields)
			if (f.name == fieldName && f.name != 0x100)
				return &f;
		return nullptr;
	}

	uint32_t Pso::U32(size_t at) const
	{
		const uint8_t* p = At(at, 4);
		return p ? uint32_t(p[0]) << 24 | uint32_t(p[1]) << 16 | uint32_t(p[2]) << 8 | p[3] : 0;
	}

	bool Pso::Read(const loader::convert::Bytes& file, std::string& error)
	{
		f_ = file;
		size_t map = 0, schema = 0;
		for (size_t at = 0; at + 8 <= f_.size();)
		{
			const uint32_t tag = U32(at), size = U32(at + 4);
			if (size < 8)
				break;
			if (tag == Tag("PSIN"))
				data_ = at;
			else if (tag == Tag("PMAP"))
				map = at;
			else if (tag == Tag("PSCH"))
				schema = at;
			at += size;
		}
		if (!map || !schema)
			return error = "not a PSO file", false;
		// PMAP: root number, block count, then (structure, offset, 0, length).
		root_ = U32(map + 8);
		const uint32_t count = U32(map + 12) >> 16;
		for (uint32_t i = 0; i < count; ++i)
			blocks_.push_back({U32(map + 16 + 16 * i), U32(map + 20 + 16 * i), U32(map + 28 + 16 * i)});
		// PSCH: count, (name, offset) per definition; a structure: type 0 / entry count, size, 0, then 12-byte entries
		// (name, type << 24 | subtype << 16 | offset, reference).
		const uint32_t definitions = U32(schema + 8);
		for (uint32_t i = 0; i < definitions; ++i)
		{
			const uint32_t name = U32(schema + 12 + 8 * i), offset = U32(schema + 16 + 8 * i);
			const size_t d = schema + offset;
			const uint32_t head = U32(d);
			if (head >> 24 != 0)
				continue; // (an enum)
			Structure s;
			s.name = name;
			s.size = U32(d + 4);
			const uint32_t entries = head & 0xFFFF;
			for (uint32_t k = 0; k < entries; ++k)
			{
				const size_t e = d + 12 + 12 * k;
				const uint32_t info = U32(e + 4);
				s.fields.push_back({U32(e), uint8_t(info >> 24), uint8_t(info >> 16), uint16_t(info), U32(e + 8)});
			}
			structures_[name] = std::move(s);
		}
		return true;
	}

	const Structure* Pso::Find(uint32_t name) const
	{
		const auto it = structures_.find(name);
		return it == structures_.end() ? nullptr : &it->second;
	}

	bool Pso::Resolve(uint32_t pointer, size_t& at, uint32_t& structure) const
	{
		const uint32_t block = pointer & 0xFFF, offset = pointer >> 12;
		if (!block || block > blocks_.size() || offset >= blocks_[block - 1].length)
			return false;
		at = data_ + blocks_[block - 1].offset + offset;
		structure = blocks_[block - 1].structure;
		return true;
	}

	Ref Pso::Root() const
	{
		if (!root_ || root_ > blocks_.size())
			return {};
		const Block& b = blocks_[root_ - 1];
		return {this, Find(b.structure), data_ + b.offset};
	}

	uint32_t Ref::U32(uint32_t field) const
	{
		const Field* f = type ? type->Find(field) : nullptr;
		return f ? pso->U32(at + f->offset) : 0;
	}

	std::string Ref::String(uint32_t field) const
	{
		const Field* f = type ? type->Find(field) : nullptr;
		if (f && f->type == kString && f->subtype == 2)
		{
			// a pointer to a C string elsewhere in the data
			size_t text = 0;
			uint32_t structure = 0;
			if (!pso->Resolve(pso->U32(at + f->offset), text, structure))
				return {};
			std::string out;
			for (const uint8_t* c = pso->At(text, 1); c && *c; c = pso->At(++text, 1))
				out += static_cast<char>(*c);
			return out;
		}
		if (!f || f->type != kString || f->subtype != 0)
			return {};
		const size_t length = f->ref >> 16;
		const uint8_t* p = pso->At(at + f->offset, length);
		if (!p)
			return {};
		return std::string(reinterpret_cast<const char*>(p), strnlen(reinterpret_cast<const char*>(p), length));
	}

	std::vector<uint32_t> Ref::U32Array(uint32_t field) const
	{
		std::vector<uint32_t> out;
		const Field* f = type ? type->Find(field) : nullptr;
		size_t data = 0;
		uint32_t structure = 0;
		if (!f || f->type != kArray || !pso->Resolve(pso->U32(at + f->offset), data, structure))
			return out;
		const uint32_t count = pso->U32(at + f->offset + 8) >> 16;
		for (uint32_t i = 0; i < count; ++i)
			out.push_back(pso->U32(data + 4 * i));
		return out;
	}

	std::vector<Ref> Ref::Array(uint32_t field) const
	{
		std::vector<Ref> out;
		const Field* f = type ? type->Find(field) : nullptr;
		size_t data = 0;
		uint32_t structure = 0;
		if (!f || f->type != kArray || !pso->Resolve(pso->U32(at + f->offset), data, structure))
			return out;
		const Structure* element = pso->Find(structure);
		if (!element)
			return out;
		const uint32_t count = pso->U32(at + f->offset + 8) >> 16;
		for (uint32_t i = 0; i < count; ++i)
			out.push_back({pso, element, data + size_t{i} * element->size});
		return out;
	}
}
