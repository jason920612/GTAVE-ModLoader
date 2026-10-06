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
	constexpr uint32_t kLegacyDrawableVersion = 165;  // .ydr and .ydd
	constexpr uint32_t kEnhancedDrawableVersion = 159;

	// Legacy resources built from drawables -> Enhanced: fragments (.yft 162 -> 171), drawables (.ydr 165 -> 159)
	// and drawable dictionaries (.ydd 165 -> 159). Everything outside the drawables keeps its layout; the
	// shaders, vertex/index buffers and declarations are rewritten (research/phase0.md §15-18). Port of
	// tools/convert_yft.py. `warnings` gets non-fatal notes.
	bool ConvertDrawableResource(const Bytes& legacy, uint32_t legacyVersion, uint32_t enhancedVersion,
	    const std::unordered_map<uint32_t, Effect>& effects, Bytes& out, std::vector<std::string>& warnings, std::string& error);
}
