#pragma once
#include <string>
#include <unordered_map>
#include <vector>

#include "effects.hpp"
#include "resource.hpp"

namespace loader::convert
{
	constexpr uint32_t kLegacyParticleVersion = 68;
	constexpr uint32_t kEnhancedParticleVersion = 71;

	// Legacy particle dictionary (.ypt 68) -> Enhanced (.ypt 71), research/phase0.md §23. The ptx objects keep their
	// layout; object type ids are renamed, each particle rule gets the Enhanced shader variable list of its
	// technique (learned from the game's own files) and one behaviour grew. The texture
	// dictionary and the models go through the drawable conversion. `warnings` gets non-fatal notes.
	// Research aid: bit 1 skips the type id renaming, 2 the shader variables, 4 the behaviours.
	extern int g_particleDebugSkip;

	bool ConvertParticleResource(const Bytes& legacy, const std::unordered_map<uint32_t, Effect>& effects, Bytes& out,
	    std::vector<std::string>& warnings, std::string& error);
}
