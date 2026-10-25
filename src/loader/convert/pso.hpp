#pragma once
#include <cstdint>
#include <string>
#include <vector>

// PSO metadata files (big-endian sections: "PSIN" the data blocks, "PMAP" the block table {structure, offset, 0,
// length} and the root's number, "PSCH" the structures; research/phase0.md §59.2, §61.13) whose root holds an array of
// structures named by a string: their entries read, changed and written back. Writing keeps every block of the file as
// it was and appends the new array and the new entries' names as two more blocks (the root then points at the new
// array); the other sections are copied, the CHKS section's length updated. Used for zonebind.ymt (zone -> population
// bindings, by zone name).
namespace loader::convert::pso
{
	class Entries
	{
	public:
		bool Read(const std::string& file, std::string& error);

		size_t Size() const { return entries_.size(); }
		const std::string& Key(size_t i) const { return entries_[i].key; }
		// The entry named `key`, or -1.
		int Find(const std::string& key) const;
		// A copy of `from`'s entry `index` named `key`: replaces the entry of that name, or is added. Returns 1 when it was
		// added, 0 when it replaced one, -1 when the files' entries differ.
		int Put(const Entries& from, size_t index, const std::string& key);
		void Clear() { entries_.clear(); }

		bool Write(std::string& out, std::string& error) const;

	private:
		struct Entry
		{
			std::string bytes; // the structure as stored (big-endian)
			std::string key;
			bool own = false;  // its name is ours (a new string), not one of the file's
		};
		std::string file_;
		size_t psin_ = 0, pmap_ = 0;
		uint32_t root_ = 0;                  // the root block (1-based)
		std::vector<uint32_t> blockStructure_, blockOffset_, blockLength_;
		uint32_t elementStructure_ = 0, elementSize_ = 0;
		uint32_t arrayOffset_ = 0;           // the array field's offset in the root structure
		std::vector<uint32_t> nameOffsets_;  // the element's string fields (pointers to C strings): the first is the key
		std::vector<Entry> entries_;
	};
}
