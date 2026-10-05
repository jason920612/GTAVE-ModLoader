#pragma once
#include <string>
#include <unordered_map>
#include <vector>

#include "effects.hpp"
#include "resource.hpp"

namespace loader::convert
{
	constexpr uint32_t kLegacyFragmentVersion = 162;
	constexpr uint32_t kEnhancedFragmentVersion = 171;

	// Legacy fragment (.yft, version 162) -> Enhanced (version 171). Port of tools/convert_yft.py;
	// formats in research/phase0.md §15-16. `warnings` gets non-fatal notes.
	bool ConvertFragment(const Bytes& legacy, const std::unordered_map<uint32_t, Effect>& effects, Bytes& out,
	    std::vector<std::string>& warnings, std::string& error);
}
