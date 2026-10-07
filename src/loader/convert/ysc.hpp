#pragma once
#include <string>
#include <vector>

#include "resource.hpp"

// Game scripts (.ysc) of the legacy game (research/phase0.md §26). Both versions run the same bytecode with the same
// native hashes; Enhanced moved global variables. Converting rewrites the operands of GLOBAL_U16 / GLOBAL_U24
// instructions with a table learned from the scripts of both games, and the globals signature in the header.
namespace loader::convert::ysc
{
	// Whether the script was compiled for the legacy game (its globals signature).
	bool IsLegacy(const Bytes& file);

	// Converts a legacy script. Globals the table does not cover are moved like the known ones around them (counted in
	// `warnings`). Scripts of other legacy builds cannot be converted.
	bool Convert(const Bytes& file, Bytes& out, std::vector<std::string>& warnings, std::string& error);
}
