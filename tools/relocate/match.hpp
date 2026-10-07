#pragma once
#include <string>
#include <vector>

#include "analysis.hpp"

// Matching the functions of two builds and relocating addresses from the old one to the new one.
namespace relocate
{
	enum class How : uint8_t
	{
		None,
		String,  // both are the only function using a string
		Exact,   // identical instruction shapes, unique in both builds
		Graph,   // called by / calling matched functions in the same place
		Similar, // best instruction similarity among nearby candidates
	};
	const char* Name(How how);

	class Matcher
	{
	public:
		Matcher(const Index& before, const Index& after);

		// Matched function in the new build (index), or -1.
		int Match(int function) const { return match_[function]; }
		How Method(int function) const { return how_[function]; }
		size_t Count(How how) const;

		struct Result
		{
			bool ok = false;
			uint32_t rva = 0;
			How how = How::None;
			std::string note;
		};
		// The new address of an old one: an instruction inside a function (the same instruction in the matched function,
		// same offset inside it) or data (what the matched code references in its place).
		Result Relocate(uint32_t rva);

	private:
		const Index& a_;
		const Index& b_;
		std::vector<int> match_;   // old -> new
		std::vector<int> reverse_; // new -> old
		std::vector<How> how_;

		bool Pair(int a, int b, How how);
		void Strings();
		void Exact();
		bool Propagate();
		int Similar(int function);
		Result InFunction(uint32_t rva, int function);
		Result InData(uint32_t rva);
	};
}
