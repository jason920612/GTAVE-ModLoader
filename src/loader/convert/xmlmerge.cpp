#include "xmlmerge.hpp"

#include <algorithm>
#include <format>
#include <unordered_map>

namespace loader::convert::xmlmerge
{
	namespace
	{
		struct Element
		{
			std::string_view tag;
			size_t start = 0, end = 0;      // the whole element, tags included
			size_t contentStart = 0, contentEnd = 0;
			std::string_view attributes;    // text between the tag name and '>'
			int parent = -1;
			int firstChild = -1;
		};

		bool IsSpace(char c)
		{
			return c == ' ' || c == '\t' || c == '\r' || c == '\n';
		}

		std::string_view Trim(std::string_view s)
		{
			while (!s.empty() && IsSpace(s.front()))
				s.remove_prefix(1);
			while (!s.empty() && IsSpace(s.back()))
				s.remove_suffix(1);
			return s;
		}

		// Elements in document order. Comments, the XML declaration and processing instructions are skipped.
		bool Parse(std::string_view x, std::vector<Element>& out)
		{
			std::vector<int> open;
			size_t i = 0;
			while ((i = x.find('<', i)) != std::string_view::npos)
			{
				if (x.compare(i, 4, "<!--") == 0)
				{
					const size_t e = x.find("-->", i + 4);
					if (e == std::string_view::npos)
						return false;
					i = e + 3;
					continue;
				}
				if (x.compare(i, 9, "<![CDATA[") == 0)
				{
					const size_t e = x.find("]]>", i + 9);
					if (e == std::string_view::npos)
						return false;
					i = e + 3;
					continue;
				}
				if (i + 1 < x.size() && (x[i + 1] == '?' || x[i + 1] == '!'))
				{
					const size_t e = x.find('>', i);
					if (e == std::string_view::npos)
						return false;
					i = e + 1;
					continue;
				}
				// Tag end, skipping quoted attribute values.
				size_t e = i + 1;
				char quote = 0;
				for (; e < x.size(); ++e)
				{
					if (quote)
					{
						if (x[e] == quote)
							quote = 0;
					}
					else if (x[e] == '"' || x[e] == '\'')
						quote = x[e];
					else if (x[e] == '>')
						break;
				}
				if (e >= x.size())
					return false;
				if (x[i + 1] == '/')
				{
					if (open.empty())
						return false;
					Element& el = out[open.back()];
					el.contentEnd = i;
					el.end = e + 1;
					open.pop_back();
					i = e + 1;
					continue;
				}
				const bool selfClosing = x[e - 1] == '/';
				size_t nameEnd = i + 1;
				while (nameEnd < e && !IsSpace(x[nameEnd]) && x[nameEnd] != '/' && x[nameEnd] != '>')
					++nameEnd;
				Element el;
				el.tag = x.substr(i + 1, nameEnd - i - 1);
				el.attributes = x.substr(nameEnd, (selfClosing ? e - 1 : e) - nameEnd);
				el.start = i;
				el.contentStart = el.contentEnd = e + 1;
				el.end = e + 1;
				el.parent = open.empty() ? -1 : open.back();
				const int index = static_cast<int>(out.size());
				if (el.parent >= 0 && out[el.parent].firstChild < 0)
					out[el.parent].firstChild = index;
				out.push_back(el);
				if (!selfClosing)
					open.push_back(index);
				i = e + 1;
			}
			return open.empty() && !out.empty();
		}

		std::string_view Attribute(std::string_view attributes, std::string_view name)
		{
			for (size_t at = attributes.find(name); at != std::string_view::npos; at = attributes.find(name, at + 1))
			{
				if (at > 0 && !IsSpace(attributes[at - 1]))
					continue;
				size_t p = at + name.size();
				while (p < attributes.size() && IsSpace(attributes[p]))
					++p;
				if (p >= attributes.size() || attributes[p] != '=')
					continue;
				++p;
				while (p < attributes.size() && IsSpace(attributes[p]))
					++p;
				if (p >= attributes.size() || (attributes[p] != '"' && attributes[p] != '\''))
					continue;
				const size_t e = attributes.find(attributes[p], p + 1);
				if (e == std::string_view::npos)
					return {};
				return attributes.substr(p + 1, e - p - 1);
			}
			return {};
		}

		std::string Lower(std::string_view s)
		{
			std::string r(s);
			for (auto& c : r)
				if (c >= 'A' && c <= 'Z')
					c = static_cast<char>(c - 'A' + 'a');
			return r;
		}

		// Key of an <Item>, or "" when it has no usable first child.
		std::string Key(std::string_view x, const std::vector<Element>& els, int index)
		{
			const Element& item = els[index];
			if (item.tag != "Item" || item.firstChild < 0)
				return {};
			const Element& first = els[item.firstChild];
			std::string_view value = Attribute(first.attributes, "value");
			if (value.empty() && first.contentEnd > first.contentStart)
				value = Trim(x.substr(first.contentStart, first.contentEnd - first.contentStart));
			if (value.empty() || value.find('<') != std::string_view::npos)
				return {};
			std::string path;
			for (int p = item.parent; p >= 0; p = els[p].parent)
				if (els[p].tag != "Item")
					path = std::string(els[p].tag) + (path.empty() ? "" : "/") + path;
			// Names are case-insensitive in the game (hashed).
			return std::format("{}|{}={}", path, first.tag, Lower(value));
		}

		// Keyed items of a document: key -> element index; keys seen more than once map to -1. Items inside a keyed item
		// are part of it (e.g. the SubHandlingData of a handling entry), not entries of their own.
		std::unordered_map<std::string, int> KeyedItems(std::string_view x, const std::vector<Element>& els)
		{
			std::unordered_map<std::string, int> keys;
			std::vector<bool> inEntry(els.size()); // the element is a keyed item or inside one
			for (int i = 0; i < static_cast<int>(els.size()); ++i)
			{
				const int parent = els[i].parent;
				if (parent >= 0 && inEntry[parent])
				{
					inEntry[i] = true;
					continue;
				}
				const std::string key = Key(x, els, i);
				if (key.empty())
					continue;
				inEntry[i] = true;
				const auto [it, added] = keys.emplace(key, i);
				if (!added)
					it->second = -1;
			}
			return keys;
		}
	}

	std::map<std::string, std::string> Entries(std::string_view xml)
	{
		std::map<std::string, std::string> out;
		std::vector<Element> els;
		if (!Parse(xml, els))
			return out;
		for (const auto& [key, index] : KeyedItems(xml, els))
			if (index >= 0)
				out.emplace(key, std::string(xml.substr(els[index].start, els[index].end - els[index].start)));
		return out;
	}

	std::string RenameValue(std::string text, std::string_view tag, std::string_view from, std::string_view to)
	{
		const std::string open = "<" + std::string(tag) + ">", close = "</" + std::string(tag) + ">";
		for (size_t at = 0; (at = text.find(open, at)) != std::string::npos;)
		{
			const size_t start = at + open.size(), end = text.find(close, start);
			if (end == std::string::npos)
				break;
			if (Lower(Trim(std::string_view(text).substr(start, end - start))) == Lower(from))
				text.replace(start, end - start, to);
			at = start;
		}
		return text;
	}

	std::string ElementText(std::string_view text, std::string_view tag)
	{
		const std::string open = "<" + std::string(tag) + ">", close = "</" + std::string(tag) + ">";
		const size_t at = text.find(open);
		if (at == std::string_view::npos)
			return {};
		const size_t end = text.find(close, at + open.size());
		return end == std::string_view::npos ? std::string() : std::string(Trim(text.substr(at + open.size(), end - at - open.size())));
	}

	int Overrides::AddFile(std::string_view xml, const std::string& source, std::string& error, const std::set<std::string>* skip)
	{
		std::vector<Element> els;
		if (!Parse(xml, els))
		{
			error = "not an XML document";
			return 0;
		}
		int added = 0;
		for (const auto& [key, index] : KeyedItems(xml, els))
		{
			if (index < 0 || (skip && skip->contains(key)))
				continue;
			const Element& el = els[index];
			const auto [it, inserted] = entries_.emplace(key, Entry{std::string(xml.substr(el.start, el.end - el.start)), source});
			if (!inserted && it->second.source != source)
				conflicts_.push_back(std::format("{} ({}, {})", key, it->second.source, source));
			added += inserted;
		}
		return added;
	}

	int Overrides::Apply(std::string_view xml, std::string& out, std::vector<std::string>* replacedKeys) const
	{
		if (entries_.empty())
			return 0;
		std::vector<Element> els;
		if (!Parse(xml, els))
			return 0;
		// Replacements in document order (items do not nest inside replaced items in practice; skip any that would).
		std::vector<std::pair<int, const Entry*>> hits;
		for (const auto& [key, index] : KeyedItems(xml, els))
			if (index >= 0)
				if (const auto it = entries_.find(key); it != entries_.end())
				{
					hits.emplace_back(index, &it->second);
					if (replacedKeys)
						replacedKeys->push_back(key);
				}
		if (hits.empty())
			return 0;
		std::sort(hits.begin(), hits.end());
		out.clear();
		out.reserve(xml.size());
		size_t at = 0;
		int replaced = 0;
		for (const auto& [index, entry] : hits)
		{
			const Element& el = els[index];
			if (el.start < at)
				continue;
			out.append(xml.substr(at, el.start - at));
			out.append(entry->text);
			at = el.end;
			++replaced;
		}
		out.append(xml.substr(at));
		return replaced;
	}
}
