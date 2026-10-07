#include "image.hpp"

#include <algorithm>
#include <cstring>
#include <fstream>

namespace relocate
{
	namespace
	{
		template<class T>
		T Read(const std::vector<uint8_t>& b, size_t at)
		{
			T v{};
			if (at + sizeof(T) <= b.size())
				memcpy(&v, b.data() + at, sizeof(T));
			return v;
		}
	}

	bool Image::Load(const std::filesystem::path& file, std::string& error)
	{
		std::ifstream in(file, std::ios::binary);
		if (!in)
		{
			error = "cannot open " + file.string();
			return false;
		}
		file_.assign(std::istreambuf_iterator<char>(in), {});
		if (Read<uint16_t>(file_, 0) != 0x5A4D)
		{
			error = "not a PE file";
			return false;
		}
		const uint32_t pe = Read<uint32_t>(file_, 0x3C);
		if (Read<uint32_t>(file_, pe) != 0x4550 || Read<uint16_t>(file_, pe + 0x18) != 0x20B)
		{
			error = "not a 64-bit PE file";
			return false;
		}
		const uint16_t count = Read<uint16_t>(file_, pe + 6);
		const uint16_t optionalSize = Read<uint16_t>(file_, pe + 0x14);
		const size_t optional = pe + 0x18;
		base_ = Read<uint64_t>(file_, optional + 0x18);
		const uint32_t exceptionRva = Read<uint32_t>(file_, optional + 0x70 + 3 * 8);
		const uint32_t exceptionSize = Read<uint32_t>(file_, optional + 0x70 + 3 * 8 + 4);
		for (uint16_t i = 0; i < count; ++i)
		{
			const size_t h = optional + optionalSize + size_t(i) * 40;
			Section s;
			s.name.assign(reinterpret_cast<const char*>(file_.data() + h), strnlen(reinterpret_cast<const char*>(file_.data() + h), 8));
			s.size = Read<uint32_t>(file_, h + 8);
			s.rva = Read<uint32_t>(file_, h + 12);
			s.fileSize = Read<uint32_t>(file_, h + 16);
			s.fileOffset = Read<uint32_t>(file_, h + 20);
			s.code = (Read<uint32_t>(file_, h + 36) & 0x20000000) != 0; // IMAGE_SCN_MEM_EXECUTE
			sections_.push_back(s);
		}
		for (uint32_t at = 0; at + 12 <= exceptionSize; at += 12)
		{
			const uint8_t* e = At(exceptionRva + at, 12);
			if (!e)
				break;
			RuntimeFunction f;
			memcpy(&f.begin, e, 4);
			memcpy(&f.end, e + 4, 4);
			if (f.begin && f.end > f.begin)
				functions_.push_back(f);
		}
		std::sort(functions_.begin(), functions_.end(), [](const RuntimeFunction& a, const RuntimeFunction& b) { return a.begin < b.begin; });
		// Chained entries (one function in several pieces) start where another one ends: keep the first piece.
		functions_.erase(std::unique(functions_.begin(), functions_.end(), [](const RuntimeFunction& a, const RuntimeFunction& b) { return a.begin == b.begin; }),
		    functions_.end());
		return true;
	}

	const Section* Image::SectionOf(uint32_t rva) const
	{
		for (const auto& s : sections_)
			if (rva >= s.rva && rva < s.rva + std::max(s.size, s.fileSize))
				return &s;
		return nullptr;
	}

	const uint8_t* Image::At(uint32_t rva, size_t size) const
	{
		const Section* s = SectionOf(rva);
		if (!s)
			return rva + size <= file_.size() && rva < 0x1000 ? file_.data() + rva : nullptr; // headers
		const uint64_t offset = uint64_t(s->fileOffset) + (rva - s->rva);
		if (rva - s->rva + size > s->fileSize || offset + size > file_.size())
			return nullptr;
		return file_.data() + offset;
	}

	bool Image::IsCode(uint32_t rva) const
	{
		const Section* s = SectionOf(rva);
		return s && s->code;
	}

	int Image::FunctionOf(uint32_t rva) const
	{
		const auto it = std::upper_bound(functions_.begin(), functions_.end(), rva, [](uint32_t v, const RuntimeFunction& f) { return v < f.begin; });
		if (it == functions_.begin())
			return -1;
		const auto& f = *std::prev(it);
		return rva < f.end ? static_cast<int>(std::prev(it) - functions_.begin()) : -1;
	}
}
