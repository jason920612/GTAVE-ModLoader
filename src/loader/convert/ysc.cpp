#include "ysc.hpp"

#include <algorithm>
#include <cstring>
#include <format>
#include <iterator>

namespace loader::convert::ysc
{
	namespace
	{
		// Globals block signature in the program header (+0x18): every script that uses globals carries the one of the
		// game build it was compiled for.
		constexpr uint32_t kLegacySignature = 0x582CE9D8;
		constexpr uint32_t kEnhancedSignature = 0xA2BE51AC;

		struct GlobalPair
		{
			uint32_t legacy, enhanced;
		};
		constexpr GlobalPair kGlobals[] = {
#define ML_YSC_GLOBALS
#include "ysc_tables.inc"
#undef ML_YSC_GLOBALS
		};

		// The program in a resource's blocks: header fields and the code pages.
		struct Program
		{
			Resource res;
			uint32_t codeSize = 0;
			std::vector<uint8_t*> pages;

			uint8_t* At(uint64_t pointer, size_t size)
			{
				const uint32_t p = static_cast<uint32_t>(pointer);
				Bytes* block = (p >> 28) == 5 ? &res.virtualBlock : (p >> 28) == 6 ? &res.physicalBlock : nullptr;
				const size_t offset = p & 0x0FFFFFFF;
				return block && offset + size <= block->size() ? block->data() + offset : nullptr;
			}

			bool Load(const Bytes& file, std::string& error)
			{
				if (!Read(file, res, error))
					return false;
				Bytes& v = res.virtualBlock;
				if (v.size() < 0x80)
				{
					error = "not a script";
					return false;
				}
				codeSize = Get<uint32_t>(v, 0x1C);
				const uint32_t pageCount = (codeSize + 0x3FFF) / 0x4000;
				const uint8_t* table = At(Get<uint64_t>(v, 0x10), size_t(pageCount) * 8);
				if (!table)
				{
					error = "damaged script";
					return false;
				}
				for (uint32_t i = 0; i < pageCount; ++i)
				{
					uint64_t page;
					memcpy(&page, table + 8 * i, 8);
					uint8_t* p = At(page, std::min<uint32_t>(0x4000, codeSize - i * 0x4000));
					if (!p)
					{
						error = "damaged script";
						return false;
					}
					pages.push_back(p);
				}
				return true;
			}

			uint32_t Signature() const { return Get<uint32_t>(res.virtualBlock, 0x18); }
			uint8_t& Code(uint32_t ip) { return pages[ip / 0x4000][ip % 0x4000]; }
		};

		// Size of the instruction at `ip` (the opcode table is the same in both versions).
		uint32_t InstructionSize(Program& p, uint32_t ip)
		{
			const uint8_t op = p.Code(ip);
			if (op <= 36 || (op >= 42 && op <= 43) || (op >= 47 && op <= 51) || op == 63 || op == 102 || op == 103 ||
			    (op >= 108 && op <= 111) || op >= 112)
				return 1;
			if (op == 37 || (op >= 52 && op <= 62) || (op >= 64 && op <= 66) || (op >= 104 && op <= 107))
				return 2;
			if (op == 38)
				return 3;
			if (op == 39 || op == 44 || op == 93 || (op >= 94 && op <= 100))
				return 4;
			if (op == 40 || op == 41)
				return 5;
			if (op == 45)
				return 5 + (ip + 4 < p.codeSize ? p.Code(ip + 4) : 0);
			if (op == 46 || (op >= 67 && op <= 92))
				return 3;
			if (op == 101)
				return 2 + 6 * (ip + 1 < p.codeSize ? p.Code(ip + 1) : 0);
			return 1;
		}

		// Enhanced index of a legacy global. Globals the game's scripts use are in the table; others are moved like the
		// known globals around them (`exact` false).
		bool MapGlobal(uint32_t g, uint32_t& out, bool& exact)
		{
			const auto it = std::lower_bound(std::begin(kGlobals), std::end(kGlobals), g, [](const GlobalPair& p, uint32_t v) { return p.legacy < v; });
			if (it != std::end(kGlobals) && it->legacy == g)
			{
				out = it->enhanced;
				exact = true;
				return true;
			}
			if (it == std::begin(kGlobals))
				return false;
			const GlobalPair& below = *std::prev(it);
			out = below.enhanced + (g - below.legacy);
			// Confident when the next known global moved by the same amount.
			exact = it != std::end(kGlobals) && int64_t(it->enhanced) - it->legacy == int64_t(below.enhanced) - below.legacy;
			return true;
		}
	}

	bool IsLegacy(const Bytes& file)
	{
		Program p;
		std::string error;
		return p.Load(file, error) && p.Signature() == kLegacySignature;
	}

	bool Convert(const Bytes& file, Bytes& out, std::vector<std::string>& warnings, std::string& error)
	{
		Program p;
		if (!p.Load(file, error))
			return false;
		if (p.Signature() != kLegacySignature)
		{
			error = std::format("the script was made for another game build (globals signature {:08X})", p.Signature());
			return false;
		}
		int guessed = 0;
		for (uint32_t ip = 0; ip < p.codeSize; ip += InstructionSize(p, ip))
		{
			const uint8_t op = p.Code(ip);
			const bool u16 = op >= 82 && op <= 84, u24 = op >= 97 && op <= 99;
			if (!u16 && !u24)
				continue;
			const uint32_t g = p.Code(ip + 1) | p.Code(ip + 2) << 8 | (u24 ? p.Code(ip + 3) << 16 : 0);
			uint32_t mapped;
			bool exact;
			if (!MapGlobal(g, mapped, exact))
			{
				error = std::format("global {} cannot be mapped", g);
				return false;
			}
			if (u16 && mapped > 0xFFFF)
			{
				error = std::format("global {} moved to {}, which does not fit the instruction", g, mapped);
				return false;
			}
			guessed += !exact;
			for (int k = 0; k < (u16 ? 2 : 3); ++k)
				p.Code(ip + 1 + k) = static_cast<uint8_t>(mapped >> (8 * k));
		}
		Put<uint32_t>(p.res.virtualBlock, 0x18, kEnhancedSignature);
		if (guessed)
			warnings.push_back(std::format("腳本中有 {} 處使用了原版腳本沒用過的全域變數，位置依相鄰的變數推算", guessed));
		return Write(p.res, out, error);
	}
}
