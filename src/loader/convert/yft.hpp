#pragma once
#include <string>
#include <unordered_map>
#include <vector>

#include "block.hpp"
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
	// Pieces shared with other converters (ypt), working on a resource being converted:
	// a texture dictionary at `dict` (legacy textures become Enhanced ones),
	bool ConvertEmbeddedTextures(Block& blk, size_t dict, std::string& error);
	// every drawable in the block (shader groups and their dictionaries, geometry, vertex and index buffers),
	bool ConvertDrawables(Block& blk, const std::unordered_map<uint32_t, Effect>& effects, std::vector<std::string>& warnings, std::string& error);
	// and the final layout: page map, pages packed, Enhanced version.
	bool FinishResource(Block& blk, uint32_t enhancedVersion, Bytes& out, std::string& error);

	bool ConvertDrawableResource(const Bytes& legacy, uint32_t legacyVersion, uint32_t enhancedVersion,
	    const std::unordered_map<uint32_t, Effect>& effects, Bytes& out, std::vector<std::string>& warnings, std::string& error);
}
