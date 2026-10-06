#include "rel.hpp"

#include <algorithm>
#include <cstring>
#include <map>
#include <set>
#include <unordered_map>

namespace loader::convert::rel
{
	namespace
	{
		struct LegacyEntry
		{
			uint32_t type, hash, crc;
		};
		constexpr LegacyEntry kLegacy[] = {
#include "rel_tables.inc"
		};

		// .rel: u32 type, u32 data size, data (u32 stamp, entries), u32 name table size, name table (u32 count,
		// u32 offsets, strings), u32 entry count, {hash, offset, size} * count, then tables of u32 data offsets
		// (fields the game fixes up when it loads the file).
		struct File
		{
			uint32_t type = 0;
			std::string data;
			std::vector<std::string> names;
			struct Entry
			{
				uint32_t hash, offset, size;
			};
			std::vector<Entry> entries;
			std::vector<std::vector<uint32_t>> tables;
		};

		struct Reader
		{
			std::string_view s;
			size_t p = 0;
			bool ok = true;
			uint32_t U32()
			{
				uint32_t v = 0;
				if (p + 4 > s.size())
				{
					ok = false;
					return 0;
				}
				memcpy(&v, s.data() + p, 4);
				p += 4;
				return v;
			}
			std::string_view Take(size_t n)
			{
				if (p + n > s.size())
				{
					ok = false;
					return {};
				}
				const auto r = s.substr(p, n);
				p += n;
				return r;
			}
		};

		bool Parse(std::string_view bytes, File& f)
		{
			Reader r{bytes};
			f.type = r.U32();
			f.data = std::string(r.Take(r.U32()));
			const std::string_view names = r.Take(r.U32());
			if (names.size() >= 4)
			{
				Reader n{names};
				const uint32_t count = n.U32();
				const size_t base = 4 + size_t(count) * 4;
				for (uint32_t i = 0; i < count && n.ok; ++i)
				{
					const uint32_t o = n.U32();
					if (base + o >= names.size())
						return false;
					const char* s = names.data() + base + o;
					f.names.emplace_back(s, strnlen(s, names.size() - base - o));
				}
			}
			const uint32_t count = r.U32();
			for (uint32_t i = 0; i < count && r.ok; ++i)
			{
				File::Entry e{r.U32(), r.U32(), r.U32()};
				if (uint64_t(e.offset) + e.size > f.data.size())
					return false;
				f.entries.push_back(e);
			}
			while (r.ok && r.p < bytes.size())
			{
				const uint32_t n = r.U32();
				std::vector<uint32_t> table;
				for (uint32_t i = 0; i < n && r.ok; ++i)
					table.push_back(r.U32());
				f.tables.push_back(std::move(table));
			}
			return r.ok && f.data.size() >= 4;
		}

		std::string Write(const File& f)
		{
			std::string out;
			const auto put = [&](uint32_t v) { out.append(reinterpret_cast<const char*>(&v), 4); };
			put(f.type);
			put(static_cast<uint32_t>(f.data.size()));
			out += f.data;
			std::string names;
			const auto putTo = [](std::string& s, uint32_t v) { s.append(reinterpret_cast<const char*>(&v), 4); };
			putTo(names, static_cast<uint32_t>(f.names.size()));
			uint32_t offset = 0;
			for (const auto& n : f.names)
			{
				putTo(names, offset);
				offset += static_cast<uint32_t>(n.size() + 1);
			}
			for (const auto& n : f.names)
				names.append(n.c_str(), n.size() + 1);
			put(static_cast<uint32_t>(names.size()));
			out += names;
			put(static_cast<uint32_t>(f.entries.size()));
			for (const auto& e : f.entries)
			{
				put(e.hash);
				put(e.offset);
				put(e.size);
			}
			for (const auto& t : f.tables)
			{
				put(static_cast<uint32_t>(t.size()));
				for (const uint32_t o : t)
					put(o);
			}
			return out;
		}

		std::string Normal(std::string_view entry)
		{
			std::string s(entry);
			for (size_t i = 1; i < 4 && i < s.size(); ++i)
				s[i] = 0;
			return s;
		}

		uint32_t Crc32(std::string_view s)
		{
			uint32_t crc = 0xFFFFFFFF;
			for (const unsigned char c : s)
			{
				crc ^= c;
				for (int k = 0; k < 8; ++k)
					crc = (crc >> 1) ^ (0xEDB88320u & (0u - (crc & 1)));
			}
			return ~crc;
		}

		// Whether `crc` is a legacy game version (base or update) of the entry.
		bool IsLegacyVersion(uint32_t type, uint32_t hash, uint32_t crc)
		{
			static const auto table = [] {
				std::unordered_multimap<uint64_t, uint32_t> m;
				for (const auto& e : kLegacy)
					m.emplace((uint64_t(e.type) << 32) | e.hash, e.crc);
				return m;
			}();
			const auto [first, last] = table.equal_range((uint64_t(type) << 32) | hash);
			return std::any_of(first, last, [&](const auto& kv) { return kv.second == crc; });
		}
	}

	std::string Merge(std::string_view game, const std::vector<Mod>& mods, int& replaced, int& added, std::string& error)
	{
		File g;
		if (!Parse(game, g))
		{
			error = "the game's file cannot be read";
			return {};
		}
		std::unordered_map<uint32_t, size_t> index;
		bool aligned = true;
		for (size_t i = 0; i < g.entries.size(); ++i)
		{
			index[g.entries[i].hash] = i;
			aligned = aligned && g.entries[i].offset % 4 == 0;
		}
		std::set<std::string> names(g.names.begin(), g.names.end());
		std::set<uint32_t> done;
		for (const Mod& mod : mods)
		{
			File m;
			if (!Parse(mod.bytes, m) || m.type != g.type)
			{
				error = mod.name + ": not a file of this kind";
				continue;
			}
			for (const auto& e : m.entries)
			{
				if (done.contains(e.hash))
					continue; // an earlier mod changed it
				const std::string_view entry(m.data.data() + e.offset, e.size);
				const auto own = index.find(e.hash);
				const std::string normal = Normal(entry);
				if (own != index.end() && normal == Normal(std::string_view(g.data).substr(g.entries[own->second].offset, g.entries[own->second].size)))
					continue; // the same as the game's
				if (IsLegacyVersion(g.type, e.hash, Crc32(normal)))
					continue; // the legacy game's own version: not changed by the mod
				// Append the mod's entry (the game's bytes 1-3 kept) and point the index at it.
				while (aligned && g.data.size() % 4)
					g.data.push_back('\0');
				const uint32_t at = static_cast<uint32_t>(g.data.size());
				std::string bytes(entry);
				if (own != index.end())
					memcpy(bytes.data() + 1, g.data.data() + g.entries[own->second].offset + 1, std::min<size_t>(3, bytes.size() - 1));
				g.data += bytes;
				for (size_t t = 0; t < m.tables.size() && t < g.tables.size(); ++t)
					for (const uint32_t o : m.tables[t])
						if (o >= e.offset && o < e.offset + e.size)
							g.tables[t].push_back(at + (o - e.offset));
				if (own != index.end())
				{
					g.entries[own->second].offset = at;
					g.entries[own->second].size = e.size;
					++replaced;
				}
				else
				{
					index[e.hash] = g.entries.size();
					g.entries.push_back({e.hash, at, e.size});
					++added;
				}
				done.insert(e.hash);
			}
			for (const auto& n : m.names)
				if (names.insert(n).second)
					g.names.push_back(n);
		}
		if (replaced + added == 0)
			return {};
		for (auto& t : g.tables)
			std::sort(t.begin(), t.end());
		// The index is sorted by the hash value rotated right by 8 bits (low byte first).
		const auto key = [](uint32_t v) { return (v >> 8) | (v << 24); };
		std::stable_sort(g.entries.begin(), g.entries.end(), [&](const File::Entry& a, const File::Entry& b) { return key(a.hash) < key(b.hash); });
		return Write(g);
	}
}
