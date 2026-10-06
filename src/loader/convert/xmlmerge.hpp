#pragma once
#include <map>
#include <set>
#include <string>
#include <string_view>
#include <vector>

// Per-entry overrides for the game's XML data files (handling.meta, vehicles.meta, carcols.meta, weapons.meta, ...).
// An entry is an <Item> element; its key is its place in the document (the tags above it) plus its first child
// element and that child's text or value attribute, e.g. "CHandlingDataMgr/HandlingData|handlingName=ADDER".
// Keys that are not unique inside a file are not overridden (such lists are plain lists, not keyed records).
namespace loader::convert::xmlmerge
{
	struct Entry
	{
		std::string text;   // the whole <Item> element
		std::string source; // mod name, for messages
	};

	// Keyed entries of a document (key -> the <Item> element's text), as AddFile sees them.
	std::map<std::string, std::string> Entries(std::string_view xml);

	// Replaces the text of the elements named `tag` whose text is `from` (case-insensitive) with `to`.
	std::string RenameValue(std::string text, std::string_view tag, std::string_view from, std::string_view to);

	// The text of the first element named `tag` inside `text` ("" when there is none).
	std::string ElementText(std::string_view text, std::string_view tag);

	class Overrides
	{
	public:
		// Adds the keyed entries of one mod file. Returns how many entries it had; `error` is set for files that are
		// not XML.
		// Entries whose key is in `skip` are left out.
		int AddFile(std::string_view xml, const std::string& source, std::string& error, const std::set<std::string>* skip = nullptr);

		bool Empty() const { return entries_.empty(); }
		size_t Size() const { return entries_.size(); }

		// Replaces the entries of `xml` that have an override. Returns the number replaced (0: `out` untouched).
		int Apply(std::string_view xml, std::string& out, std::vector<std::string>* replacedKeys = nullptr) const;

		// Entries that replaced another mod's entry with the same key (first mod wins), as "key (mod a, mod b)".
		const std::vector<std::string>& Conflicts() const { return conflicts_; }

	private:
		std::map<std::string, Entry> entries_;
		std::vector<std::string> conflicts_;
	};
}
