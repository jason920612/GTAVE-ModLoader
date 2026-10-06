#pragma once
#include <string>
#include <vector>

#include "resource.hpp"

namespace loader::convert
{
	// Ped variation data (.ymt, version 2) lists which components a ped has; the game draws them from the
	// drawable dictionary (.ydd) of the same name ("uppr_000_u", ...). The legacy game skipped components whose
	// drawables are missing; Enhanced then draws nothing at all. Components without any drawable are marked
	// unavailable (degraded, with a warning). `dictionary` is the ped's .ydd file. Returns whether `ymt` changed.
	bool DropMissingComponents(Bytes& ymt, const Bytes& dictionary, const std::string& name, std::vector<std::string>& warnings);
}
