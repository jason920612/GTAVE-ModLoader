#pragma once
#include <array>
#include <cstdint>
#include <string>
#include <unordered_map>
#include <vector>

#include "image.hpp"

// Per-function features of an image (research/phase0.md §27): what stays the same when the game is rebuilt, so
// functions can be found again in another build.
namespace relocate
{
	enum class Ref : uint8_t
	{
		None,
		Call,   // direct call
		Jump,   // direct jump (also conditional)
		Data,   // rip-relative memory operand or address immediate
	};

	struct Instruction
	{
		uint32_t rva = 0;
		uint8_t length = 0;
		uint16_t mnemonic = 0;
		Ref ref = Ref::None;
		uint32_t target = 0; // RVA for refs
		uint64_t shape = 0;  // the instruction without addresses (registers, struct offsets, small constants kept)
		std::vector<uint64_t> values; // constants and struct offsets in the instruction (compiler independent)
	};

	// Decodes the instructions in [begin, end) (stops at undecodable bytes).
	std::vector<Instruction> Decode(const Image& image, uint32_t begin, uint32_t end);

	// Intel syntax text of one instruction.
	std::string Format(const Image& image, uint32_t rva);

	struct FunctionInfo
	{
		uint32_t begin = 0, end = 0;
		uint32_t instructions = 0;
		uint64_t exact = 0;                // hash of the instruction shapes
		std::vector<uint32_t> callees;     // call targets (function starts), in call order
		std::vector<int> calleeIndex;      // the same as function indices (-1 when not a known function)
		std::vector<int> callers;          // function indices
		std::vector<uint32_t> dataRefs;    // rip-relative data targets
		std::vector<int> strings;          // string ids referenced
		std::array<uint32_t, 16> minhash{}; // of instruction mnemonic 3-grams, for similarity
		std::vector<uint64_t> values;       // sorted unique constants, struct offsets and read-only data values used
		std::array<uint32_t, 16> valueHash{}; // minhash of `values`
	};

	class Index
	{
	public:
		bool Build(const std::filesystem::path& file, std::string& error);

		const Image& Img() const { return image_; }
		const std::vector<FunctionInfo>& Functions() const { return functions_; }
		const std::vector<std::string>& Strings() const { return strings_; }
		// Functions referencing a string id.
		const std::vector<int>& StringUsers(int id) const { return stringUsers_[id]; }
		int StringId(const std::string& text) const;
		int FunctionAt(uint32_t begin) const;

	private:
		Image image_;
		std::vector<FunctionInfo> functions_;
		std::unordered_map<uint32_t, int> byBegin_;
		std::vector<std::string> strings_;
		std::unordered_map<std::string, int> stringIds_;
		std::vector<std::vector<int>> stringUsers_;
	};

	// Similarity of two functions' instruction 3-gram sets (0..1): good within one compiler.
	double Similarity(const FunctionInfo& a, const FunctionInfo& b);
	// Similarity of what two functions use (constants, struct offsets, data values, call count; 0..1): survives a change
	// of compiler or optimisation.
	double ValueSimilarity(const FunctionInfo& a, const FunctionInfo& b);
}
