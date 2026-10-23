#pragma once
#include <filesystem>
#include <map>
#include <memory>
#include <string>
#include <vector>

#include "convert/archive.hpp"

// The game's files as story mode sees them (research/phase0.md §58.3): the base archives, the title update
// (update.rpf, update2.rpf: x64\<path> replaces <path>), then the DLC packs in dlclist.xml order with the content
// change sets story mode executes (every group but the online map's, GROUP_MAP). An overlay archive replaces the
// archive at the same path; a file name the streaming system knows twice is the later one.
namespace ue5export
{
	class GameFiles
	{
	public:
		struct File
		{
			std::shared_ptr<const loader::convert::Archive> archive;
			uint32_t node = 0;
			std::string path; // where it came from (for messages)
		};

		bool Open(const std::filesystem::path& gameDir, const loader::convert::Decryptor& decrypt, std::string& error);

		// The file the game uses under this name (lower case, with extension), or nullptr.
		const File* Find(const std::string& name) const;
		const std::map<std::string, File>& Files() const { return byName_; }
		// Every version of a data file (.meta) in the active archives, in mount order (the DLC packs' gtxd.meta...).
		std::vector<File> AllOf(const std::string& name) const;
		bool Read(const File& file, loader::convert::Bytes& out, std::string& error) const;

		std::vector<std::string> warnings;
		size_t activeArchives = 0;

	private:
		struct Mounted
		{
			std::shared_ptr<const loader::convert::Archive> archive;
			std::string source;
		};
		// Archives by their path in the game's file system ("levels/gta5/_hills/cityhills_01/ch1_05.rpf"), in mount
		// order; loose files (not inside one of these) by path too.
		std::map<std::string, Mounted> archives_;
		std::vector<std::string> order_;
		std::map<std::string, File> loose_;
		std::map<std::string, File> byName_;
		std::map<std::string, std::vector<File>> all_;
		const loader::convert::Decryptor* decrypt_ = nullptr;

		void Mount(const std::shared_ptr<const loader::convert::Archive>& archive, uint32_t node, const std::string& prefix,
		    const std::string& source, bool topLevel);
		void Register(const std::string& path, const std::shared_ptr<const loader::convert::Archive>& archive, const std::string& source);
	};
}
