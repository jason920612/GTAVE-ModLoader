// Game script bytecode (YSC) decoding: instruction lengths and operands, enough to walk a program's code.
// Opcode numbers and operand layouts come from the game's own interpreter (research/phase0.md §22).
#pragma once
#include <cstdint>
#include <cstring>
#include <span>

namespace ml::script
{
	enum Op : uint8_t
	{
		NOP = 0,
		PUSH_U8 = 37,
		PUSH_U8_U8 = 38,
		PUSH_U8_U8_U8 = 39,
		PUSH_U32 = 40,
		PUSH_F = 41,
		NATIVE = 44, // u8 (args << 2 | returns), u16 big-endian native index
		ENTER = 45,  // u8 params, u16 frame size, u8 name length, name
		LEAVE = 46,  // u8 params, u8 returns
		PUSH_S16 = 67,
		J = 85,       // s16 relative to the next instruction
		JZ = 86,
		ILE_JZ = 92, // 87..92: compare-and-jump, s16 like J
		CALL = 93,   // u24 absolute address
		GLOBAL_U24 = 97,
		GLOBAL_U24_LOAD = 98,
		GLOBAL_U24_STORE = 99,
		PUSH_U24 = 100,
		SWITCH = 101, // u8 count, then count * (i32 value, s16 relative jump)
		GLOBAL_U16 = 82,
		GLOBAL_U16_LOAD = 83,
		GLOBAL_U16_STORE = 84,
		STATIC_U24 = 94,
		LAST_VALID = 130,
	};

	struct Instruction
	{
		uint32_t address = 0;
		uint32_t length = 0; // 0 = invalid / truncated
		uint8_t op = 0;
		int64_t operand = 0; // immediate, global or static index, jump/call target, native index, ENTER params
	};

	inline bool IsJump(uint8_t op) { return op >= J && op <= ILE_JZ; }

	// Decodes the instruction at `address`. Jump operands are made absolute.
	inline Instruction Decode(std::span<const uint8_t> code, uint32_t address)
	{
		Instruction in{address};
		if (address >= code.size())
			return in;
		const uint8_t* p = code.data() + address;
		const size_t left = code.size() - address;
		in.op = p[0];
		const auto u16 = [&](size_t o) { return static_cast<uint32_t>(p[o] | p[o + 1] << 8); };
		const auto u24 = [&](size_t o) { return static_cast<uint32_t>(p[o] | p[o + 1] << 8 | p[o + 2] << 16); };
		uint32_t length = 1;
		switch (in.op)
		{
		case PUSH_U8: case 52: case 53: case 54: case 55: case 56: case 57: case 58: case 59: case 60: case 61: case 62: case 64: case 65:
		case 66: case 104: case 105: case 106: case 107:
			length = 2;
			if (left >= 2)
				in.operand = p[1];
			break;
		case PUSH_U8_U8: length = 3; break;
		case PUSH_U8_U8_U8: length = 4; break;
		case PUSH_U32: case PUSH_F:
			length = 5;
			if (left >= 5)
			{
				int32_t v;
				std::memcpy(&v, p + 1, 4);
				in.operand = v;
			}
			break;
		case NATIVE:
			length = 4;
			if (left >= 4)
				in.operand = p[2] << 8 | p[3];
			break;
		case ENTER:
			length = left >= 5 ? 5u + p[4] : 5u;
			if (left >= 2)
				in.operand = p[1];
			break;
		case LEAVE: length = 3; break;
		case 67: case 68: case 69: case 70: case 71: case 72:
			length = 3;
			if (left >= 3)
				in.operand = static_cast<int16_t>(u16(1));
			break;
		case 73: case 74: case 75: case 76: case 77: case 78: case 79: case 80: case 81: case GLOBAL_U16: case GLOBAL_U16_LOAD: case GLOBAL_U16_STORE:
			length = 3;
			if (left >= 3)
				in.operand = u16(1);
			break;
		case J: case JZ: case 87: case 88: case 89: case 90: case 91: case ILE_JZ:
			length = 3;
			if (left >= 3)
				in.operand = static_cast<int64_t>(address) + 3 + static_cast<int16_t>(u16(1));
			break;
		case CALL: case STATIC_U24: case 95: case 96: case GLOBAL_U24: case GLOBAL_U24_LOAD: case GLOBAL_U24_STORE: case PUSH_U24:
			length = 4;
			if (left >= 4)
				in.operand = u24(1);
			break;
		case SWITCH:
			length = left >= 2 ? 2u + 6u * p[1] : 2u;
			if (left >= 2)
				in.operand = p[1];
			break;
		default:
			if (in.op > LAST_VALID)
				return in;
		}
		if (length > left)
			return in;
		in.length = length;
		return in;
	}

	// SWITCH case `index`: absolute jump target.
	inline uint32_t SwitchTarget(std::span<const uint8_t> code, uint32_t address, uint32_t index)
	{
		const uint32_t at = address + 2 + 6 * index;
		const auto rel = static_cast<int16_t>(code[at + 4] | code[at + 5] << 8);
		return static_cast<uint32_t>(at + 6 + rel);
	}

	inline bool IsGlobalStore(const Instruction& in) { return in.op == GLOBAL_U16_STORE || in.op == GLOBAL_U24_STORE; }
}
