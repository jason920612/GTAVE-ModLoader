#pragma once
#include <array>
#include <filesystem>
#include <string>
#include <vector>

#include "../ue5export/gamefiles.hpp"
#include "roads.hpp"

// The road network's region files with new roads (research/phase0.md §61.7, §61.16): every base region of the game
// (nodes0..1023.ynd) written again for the loader's region grid (1024 m regions, 32 x 32 from -16384 m; positions
// relative to the region's south-west corner outside +-8192 m) with the game's nodes - except those inside `cut`
// (the new land: boat routes over it) and the links to them - and the new roads' nodes (made of a plain two-way
// street node of the game, its flags kept). Cayo Perico's set (1024+) is left as it is.
namespace worldgen::paths
{
	struct Stats
	{
		size_t regions = 0, gameNodes = 0, dropped = 0, newNodes = 0, links = 0;
	};

	bool Write(const ue5export::GameFiles& files, const std::array<float, 4>& cut, const std::vector<roads::PathNode>& nodes,
	    const std::vector<roads::PathLink>& links, const std::filesystem::path& out, Stats& stats, std::string& error);
}
