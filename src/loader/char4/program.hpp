#pragma once
#include <cstdint>
#include <string>
#include <vector>

#include "../convert/resource.hpp"

// A game script (.ysc) read into parts that can be changed (code, natives, statics, strings, global pages) and written
// back as a new resource laid out from scratch (research/char4-design.md §3.4, phase0.md §55.6 U2; the C++ form of
// tools/ysc_write.py).
namespace loader::char4
{
	using convert::Bytes;

	constexpr uint32_t kCodePage = 0x4000; // bytes in a code or string page

	// Opcodes the rewrite rules use (the table is the same in the legacy and Enhanced games).
	namespace op
	{
		constexpr uint8_t Nop = 0, Ieq = 8, Ine = 9, Igt = 10, Ige = 11, Ilt = 12, Ile = 13;
		constexpr uint8_t PushU8 = 37, PushU32 = 40, Dup = 42, Drop = 43, Native = 44, Enter = 45, Leave = 46;
		constexpr uint8_t ArrayU8 = 52, ArrayU8Load = 53, ArrayU8Store = 54, LocalU8Load = 56, LocalU8Store = 57, IoffsetU8Load = 65;
		constexpr uint8_t PushS16 = 67, ArrayU16 = 73, ArrayU16Store = 75, GlobalU16 = 82, GlobalU16Store = 84;
		constexpr uint8_t J = 85, Jz = 86, IeqJz = 87, IneJz = 88, IgtJz = 89, IgeJz = 90, IltJz = 91, IleJz = 92;
		constexpr uint8_t Call = 93, GlobalU24 = 97, GlobalU24Store = 99, PushU24 = 100, Switch = 101;
		constexpr uint8_t PushM1 = 112, Push0 = 113, Push3 = 116, Push4 = 117, Push7 = 120;
		inline bool IsJump(uint8_t o) { return o >= J && o <= IleJz; }
		// PUSH_CONST_0..7 (and -1): the value, or false.
		inline bool SmallPush(uint8_t o, int& value)
		{
			if (o < PushM1 || o > Push7)
				return false;
			value = o - Push0;
			return true;
		}
	}

	struct Instruction
	{
		uint32_t at;
		uint8_t op;
		uint32_t size;
	};
	// Size of the instruction at `ip`.
	uint32_t InstructionSize(const Bytes& code, uint32_t ip);
	// Every instruction of [start, end) in order.
	std::vector<Instruction> Decode(const Bytes& code, uint32_t start = 0, uint32_t end = UINT32_MAX);

	inline uint32_t U24(const Bytes& code, uint32_t at) { return code[at] | code[at + 1] << 8 | code[at + 2] << 16; }
	inline int16_t S16(const Bytes& code, uint32_t at) { return static_cast<int16_t>(code[at] | code[at + 1] << 8); }
	inline int32_t S32(const Bytes& code, uint32_t at)
	{
		return static_cast<int32_t>(code[at] | code[at + 1] << 8 | code[at + 2] << 16 | uint32_t{code[at + 3]} << 24);
	}
	// A relative jump's target (from the end of the 3-byte instruction).
	inline uint32_t JumpTarget(const Bytes& code, uint32_t at) { return at + 3 + S16(code, at + 1); }

	struct SwitchCase
	{
		int32_t value;
		uint32_t target;
	};
	std::vector<SwitchCase> SwitchCases(const Bytes& code, uint32_t at);

	struct Program
	{
		uint32_t version = 0, virtualFlags = 0, physicalFlags = 0;
		Bytes header;                  // the first 0x80 bytes, kept except the fields Write sets
		Bytes code;
		std::vector<uint64_t> natives; // decoded (the game's hashes)
		std::vector<uint64_t> statics;
		Bytes strings;
		uint32_t globalsWord = 0;      // block << 18 | slot count
		std::vector<uint64_t> globals; // initial values of the block the script owns
		std::string name;

		bool Read(const Bytes& file, std::string& error);
		bool Write(Bytes& out, std::string& error) const;

		// Index of `hash` in the native table, added when missing.
		uint16_t NativeIndex(uint64_t hash);
	};

	// Functions: the address of every ENTER, in order, and the function holding an address.
	struct Functions
	{
		std::vector<uint32_t> starts;
		uint32_t codeSize = 0;
		explicit Functions(const std::vector<Instruction>& ins, uint32_t size);
		uint32_t Owner(uint32_t at) const;
		uint32_t End(uint32_t start) const; // the next function's start (or the code's end)
	};
}
