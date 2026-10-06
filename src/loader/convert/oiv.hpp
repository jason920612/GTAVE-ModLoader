#pragma once
#include <filesystem>
#include <string>
#include <vector>

#include "resource.hpp"

// .oiv packages (OpenIV's install packages; read with our own code): a zip archive holding assembly.xml and a
// content folder. assembly.xml lists <add source="content file">target path in the game</add> entries, possibly
// inside <archive path="..."> elements (files inside the game's archives). We only need what each file is:
// a DLC pack (target ...\dlc.rpf) or a file that replaces / adds a game file by its name. Text and XML edits of
// game files cannot be applied; editing dlclist.xml is not needed (the loader registers the packs itself).
namespace loader::convert
{
	struct OivFile
	{
		std::string source; // path inside the package
		std::string target; // where OpenIV would put it
		std::string name;   // file name of the target, lower case
		bool dlcPack = false; // target is a DLC pack's dlc.rpf
		std::string packName; // for DLC packs: the folder name above dlc.rpf
	};

	struct Oiv
	{
		std::string title;
		std::vector<OivFile> files;
		std::vector<std::string> warnings; // edits that are not applied
	};

	// Reads the package index (assembly.xml). Returns false with `error` when it is not a readable package.
	bool ReadOiv(const std::filesystem::path& file, Oiv& out, std::string& error);

	// Reads one file of the package.
	bool ReadOivFile(const std::filesystem::path& file, const std::string& source, Bytes& out, std::string& error);
}
