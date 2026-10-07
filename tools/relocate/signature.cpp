#include "signature.hpp"

#include <Zydis/Zydis.h>

#include <cstring>
#include <format>
#include <sstream>

namespace relocate
{
	std::optional<Pattern> Pattern::Parse(const std::string& text)
	{
		Pattern p;
		std::istringstream in(text);
		for (std::string token; in >> token;)
		{
			if (token == "?" || token == "??")
				p.bytes.push_back(-1);
			else if (token.size() == 2 && isxdigit(static_cast<unsigned char>(token[0])) && isxdigit(static_cast<unsigned char>(token[1])))
				p.bytes.push_back(std::stoi(token, nullptr, 16));
			else
				return std::nullopt;
		}
		if (p.bytes.empty())
			return std::nullopt;
		return p;
	}

	std::string Pattern::Text() const
	{
		std::string s;
		for (const int b : bytes)
		{
			if (!s.empty())
				s += ' ';
			s += b < 0 ? std::string("?") : std::format("{:02X}", b);
		}
		return s;
	}

	std::vector<uint32_t> Find(const Image& image, const Pattern& pattern, size_t limit)
	{
		std::vector<uint32_t> out;
		const size_t n = pattern.bytes.size();
		// Anchor on the first fixed byte.
		size_t anchor = 0;
		while (anchor < n && pattern.bytes[anchor] < 0)
			++anchor;
		if (anchor == n)
			return out;
		for (const auto& s : image.Sections())
		{
			if (!s.code)
				continue;
			const uint32_t size = std::min(s.size, s.fileSize);
			const uint8_t* data = image.At(s.rva, size);
			if (!data || size < n)
				continue;
			const uint8_t first = static_cast<uint8_t>(pattern.bytes[anchor]);
			for (const uint8_t* p = data + anchor; p < data + size - n + anchor + 1;)
			{
				p = static_cast<const uint8_t*>(memchr(p, first, (data + size - n + anchor + 1) - p));
				if (!p)
					break;
				const uint8_t* start = p - anchor;
				size_t k = 0;
				while (k < n && (pattern.bytes[k] < 0 || start[k] == pattern.bytes[k]))
					++k;
				if (k == n)
				{
					out.push_back(s.rva + static_cast<uint32_t>(start - data));
					if (out.size() >= limit)
						return out;
				}
				++p;
			}
		}
		return out;
	}

	std::string MakeSignature(const Image& image, uint32_t rva, size_t maxBytes)
	{
		ZydisDecoder decoder;
		ZydisDecoderInit(&decoder, ZYDIS_MACHINE_MODE_LONG_64, ZYDIS_STACK_WIDTH_64);
		const uint64_t base = image.Base();
		Pattern p;
		for (uint32_t at = rva; p.bytes.size() < maxBytes;)
		{
			const uint8_t* code = image.At(at, 15);
			ZydisDecodedInstruction ins;
			ZydisDecodedOperand ops[ZYDIS_MAX_OPERAND_COUNT];
			if (!code || !ZYAN_SUCCESS(ZydisDecoderDecodeFull(&decoder, code, 15, &ins, ops)))
				break;
			std::vector<int> bytes(code, code + ins.length);
			const auto mask = [&](uint8_t offset, uint8_t bits) {
				for (int k = 0; k < bits / 8; ++k)
					bytes[offset + k] = -1;
			};
			if (ins.raw.disp.size && (ins.attributes & ZYDIS_ATTRIB_HAS_MODRM))
				for (uint8_t o = 0; o < ins.operand_count_visible; ++o)
					if (ops[o].type == ZYDIS_OPERAND_TYPE_MEMORY && ops[o].mem.base == ZYDIS_REGISTER_RIP)
						mask(ins.raw.disp.offset, ins.raw.disp.size);
			for (int k = 0; k < 2; ++k)
				if (ins.raw.imm[k].size &&
				    (ins.raw.imm[k].is_relative || (ins.raw.imm[k].value.u >= base && ins.raw.imm[k].value.u < base + 0x10000000)))
					mask(ins.raw.imm[k].offset, ins.raw.imm[k].size);
			p.bytes.insert(p.bytes.end(), bytes.begin(), bytes.end());
			at += ins.length;
			int fixed = 0;
			for (const int b : p.bytes)
				fixed += b >= 0;
			if (fixed >= 6 && Find(image, p, 2).size() == 1)
				return p.Text();
		}
		return {};
	}
}
