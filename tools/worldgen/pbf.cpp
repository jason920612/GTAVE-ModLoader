#include "pbf.hpp"

#include <cstring>
#include <fstream>
#include <string_view>
#include <vector>

#include "convert/resource.hpp"

namespace worldgen::pbf
{
	namespace
	{
		// Protocol buffers, as far as the format needs them.
		struct Reader
		{
			const uint8_t* p = nullptr;
			const uint8_t* end = nullptr;

			bool More() const { return p < end; }
			uint64_t Varint()
			{
				uint64_t v = 0;
				for (int shift = 0; p < end && shift < 64; shift += 7)
				{
					const uint8_t b = *p++;
					v |= static_cast<uint64_t>(b & 0x7F) << shift;
					if (!(b & 0x80))
						break;
				}
				return v;
			}
			static int64_t Zigzag(uint64_t v) { return static_cast<int64_t>(v >> 1) ^ -static_cast<int64_t>(v & 1); }
			void Key(uint32_t& field, uint32_t& wire)
			{
				const uint64_t k = Varint();
				field = static_cast<uint32_t>(k >> 3), wire = static_cast<uint32_t>(k & 7);
			}
			Reader Sub()
			{
				const uint64_t n = Varint();
				const uint8_t* start = p;
				p = static_cast<size_t>(end - p) < n ? end : p + n;
				return {start, p};
			}
			void Skip(uint32_t wire)
			{
				switch (wire)
				{
				case 0: Varint(); break;
				case 1: p += 8; break;
				case 2: Sub(); break;
				case 5: p += 4; break;
				default: p = end; break;
				}
			}
		};

		template <typename F>
		void Packed(Reader r, F f)
		{
			while (r.More())
				f(r.Varint());
		}

		struct Block
		{
			std::vector<std::string_view> strings;
			int64_t granularity = 100, latOffset = 0, lonOffset = 0;
		};

		osm::Tags TagsOf(const Block& b, const std::vector<uint32_t>& keys, const std::vector<uint32_t>& vals)
		{
			osm::Tags t;
			for (size_t i = 0; i < keys.size() && i < vals.size(); ++i)
				if (keys[i] < b.strings.size() && vals[i] < b.strings.size())
					t.emplace(std::string(b.strings[keys[i]]), std::string(b.strings[vals[i]]));
			return t;
		}
	}

	bool Load(const std::filesystem::path& file, double south, double west, double north, double east, double margin, osm::Data& out, std::string& error)
	{
		std::ifstream in(file, std::ios::binary);
		if (!in)
			return error = "cannot read " + file.string(), false;
		out.south = south, out.west = west, out.north = north, out.east = east;
		const double s0 = south - margin, s1 = north + margin, w0 = west - margin, w1 = east + margin;
		std::string header, blob;
		loader::convert::Bytes data;
		size_t blocks = 0;
		while (true)
		{
			uint8_t len[4];
			if (!in.read(reinterpret_cast<char*>(len), 4))
				break;
			const uint32_t hl = static_cast<uint32_t>(len[0]) << 24 | len[1] << 16 | len[2] << 8 | len[3];
			header.resize(hl);
			if (hl > (1u << 20) || !in.read(header.data(), hl))
				return error = "a broken block header", false;
			Reader h{reinterpret_cast<const uint8_t*>(header.data()), reinterpret_cast<const uint8_t*>(header.data()) + hl};
			std::string type;
			uint64_t size = 0;
			while (h.More())
			{
				uint32_t f, wt;
				h.Key(f, wt);
				if (f == 1 && wt == 2)
				{
					const Reader s = h.Sub();
					type.assign(reinterpret_cast<const char*>(s.p), s.end - s.p);
				}
				else if (f == 3 && wt == 0)
					size = h.Varint();
				else
					h.Skip(wt);
			}
			blob.resize(size);
			if (size > (64u << 20) || !in.read(blob.data(), static_cast<std::streamsize>(size)))
				return error = "a broken block", false;
			if (type != "OSMData")
				continue;
			// the blob: raw, or zlib (its 2-byte header and adler32 around raw deflate)
			Reader b{reinterpret_cast<const uint8_t*>(blob.data()), reinterpret_cast<const uint8_t*>(blob.data()) + size};
			const uint8_t* raw = nullptr;
			size_t rawLength = 0, rawSize = 0;
			bool zlib = false;
			while (b.More())
			{
				uint32_t f, wt;
				b.Key(f, wt);
				if ((f == 1 || f == 3) && wt == 2)
				{
					const Reader s = b.Sub();
					raw = s.p, rawLength = s.end - s.p, zlib = f == 3;
				}
				else if (f == 2 && wt == 0)
					rawSize = b.Varint();
				else
					b.Skip(wt);
			}
			if (!raw)
				return error = "a block with no data (not raw or zlib)", false;
			if (zlib)
			{
				data.assign(rawSize, 0);
				if (rawLength < 6 || !loader::convert::Inflate(raw + 2, rawLength - 6, data))
					return error = "a block that does not inflate", false;
			}
			else
				data.assign(raw, raw + rawLength);
			++blocks;
			// the primitive block: its strings and scale, then its groups
			Reader pb{data.data(), data.data() + data.size()};
			Block blk;
			std::vector<Reader> groups;
			while (pb.More())
			{
				uint32_t f, wt;
				pb.Key(f, wt);
				if (f == 1 && wt == 2)
				{
					Reader st = pb.Sub();
					while (st.More())
					{
						uint32_t sf, swt;
						st.Key(sf, swt);
						if (sf == 1 && swt == 2)
						{
							const Reader s = st.Sub();
							blk.strings.emplace_back(reinterpret_cast<const char*>(s.p), s.end - s.p);
						}
						else
							st.Skip(swt);
					}
				}
				else if (f == 2 && wt == 2)
					groups.push_back(pb.Sub());
				else if (f == 17 && wt == 0)
					blk.granularity = static_cast<int64_t>(pb.Varint());
				else if (f == 19 && wt == 0)
					blk.latOffset = static_cast<int64_t>(pb.Varint());
				else if (f == 20 && wt == 0)
					blk.lonOffset = static_cast<int64_t>(pb.Varint());
				else
					pb.Skip(wt);
			}
			const auto lat = [&](int64_t v) { return 1e-9 * static_cast<double>(blk.latOffset + blk.granularity * v); };
			const auto lon = [&](int64_t v) { return 1e-9 * static_cast<double>(blk.lonOffset + blk.granularity * v); };
			const auto inside = [&](double la, double lo) { return la >= s0 && la <= s1 && lo >= w0 && lo <= w1; };
			for (Reader g : groups)
				while (g.More())
				{
					uint32_t f, wt;
					g.Key(f, wt);
					if (wt != 2)
					{
						g.Skip(wt);
						continue;
					}
					Reader e = g.Sub();
					if (f == 2) // dense nodes
					{
						std::vector<int64_t> ids, lats, lons;
						std::vector<uint32_t> kv;
						while (e.More())
						{
							uint32_t ef, ewt;
							e.Key(ef, ewt);
							if (ewt != 2)
							{
								e.Skip(ewt);
								continue;
							}
							const Reader r = e.Sub();
							int64_t acc = 0;
							if (ef == 1)
								Packed(r, [&](uint64_t v) { ids.push_back(acc += Reader::Zigzag(v)); });
							else if (ef == 8)
								Packed(r, [&](uint64_t v) { lats.push_back(acc += Reader::Zigzag(v)); });
							else if (ef == 9)
								Packed(r, [&](uint64_t v) { lons.push_back(acc += Reader::Zigzag(v)); });
							else if (ef == 10)
								Packed(r, [&](uint64_t v) { kv.push_back(static_cast<uint32_t>(v)); });
						}
						size_t k = 0;
						for (size_t i = 0; i < ids.size() && i < lats.size() && i < lons.size(); ++i)
						{
							std::vector<uint32_t> keys, vals;
							while (k < kv.size() && kv[k] != 0)
							{
								keys.push_back(kv[k]);
								vals.push_back(k + 1 < kv.size() ? kv[k + 1] : 0);
								k += 2;
							}
							++k; // (the 0 ending this node's pairs)
							const double la = lat(lats[i]), lo = lon(lons[i]);
							if (!inside(la, lo))
								continue;
							out.nodes[ids[i]] = {la, lo};
							if (!keys.empty())
								out.nodeTags[ids[i]] = TagsOf(blk, keys, vals);
						}
					}
					else if (f == 1) // a plain node
					{
						int64_t id = 0, la = 0, lo = 0;
						std::vector<uint32_t> keys, vals;
						while (e.More())
						{
							uint32_t ef, ewt;
							e.Key(ef, ewt);
							if (ef == 1 && ewt == 0)
								id = Reader::Zigzag(e.Varint());
							else if (ef == 8 && ewt == 0)
								la = Reader::Zigzag(e.Varint());
							else if (ef == 9 && ewt == 0)
								lo = Reader::Zigzag(e.Varint());
							else if ((ef == 2 || ef == 3) && ewt == 2)
								Packed(e.Sub(), [&](uint64_t v) { (ef == 2 ? keys : vals).push_back(static_cast<uint32_t>(v)); });
							else
								e.Skip(ewt);
						}
						if (inside(lat(la), lon(lo)))
						{
							out.nodes[id] = {lat(la), lon(lo)};
							if (!keys.empty())
								out.nodeTags[id] = TagsOf(blk, keys, vals);
						}
					}
					else if (f == 3) // a way: kept when it touches a node kept
					{
						int64_t id = 0;
						std::vector<uint32_t> keys, vals;
						std::vector<int64_t> refs;
						while (e.More())
						{
							uint32_t ef, ewt;
							e.Key(ef, ewt);
							if (ef == 1 && ewt == 0)
								id = static_cast<int64_t>(e.Varint());
							else if ((ef == 2 || ef == 3) && ewt == 2)
								Packed(e.Sub(), [&](uint64_t v) { (ef == 2 ? keys : vals).push_back(static_cast<uint32_t>(v)); });
							else if (ef == 8 && ewt == 2)
							{
								int64_t acc = 0;
								Packed(e.Sub(), [&](uint64_t v) { refs.push_back(acc += Reader::Zigzag(v)); });
							}
							else
								e.Skip(ewt);
						}
						bool touches = false;
						for (const int64_t r : refs)
							if (out.nodes.contains(r))
							{
								touches = true;
								break;
							}
						if (touches)
							out.ways[id] = {std::move(refs), TagsOf(blk, keys, vals)};
					}
					else if (f == 4) // a relation: kept when a member is
					{
						int64_t id = 0;
						std::vector<uint32_t> keys, vals, roles, types;
						std::vector<int64_t> members;
						while (e.More())
						{
							uint32_t ef, ewt;
							e.Key(ef, ewt);
							if (ef == 1 && ewt == 0)
								id = static_cast<int64_t>(e.Varint());
							else if ((ef == 2 || ef == 3 || ef == 8 || ef == 10) && ewt == 2)
								Packed(e.Sub(), [&](uint64_t v) {
									(ef == 2 ? keys : ef == 3 ? vals : ef == 8 ? roles : types).push_back(static_cast<uint32_t>(v));
								});
							else if (ef == 9 && ewt == 2)
							{
								int64_t acc = 0;
								Packed(e.Sub(), [&](uint64_t v) { members.push_back(acc += Reader::Zigzag(v)); });
							}
							else
								e.Skip(ewt);
						}
						osm::Relation rel;
						bool touches = false;
						for (size_t i = 0; i < members.size(); ++i)
						{
							const uint32_t t = i < types.size() ? types[i] : 0;
							const char kind = t == 0 ? 'n' : t == 1 ? 'w' : 'r';
							const std::string role = i < roles.size() && roles[i] < blk.strings.size() ? std::string(blk.strings[roles[i]]) : std::string();
							rel.members.push_back({kind, members[i], role});
							touches |= (kind == 'w' && out.ways.contains(members[i])) || (kind == 'n' && out.nodes.contains(members[i]));
						}
						if (touches)
						{
							rel.tags = TagsOf(blk, keys, vals);
							out.relations[id] = std::move(rel);
						}
					}
				}
		}
		if (!blocks)
			return error = file.string() + " is not an OpenStreetMap extract", false;
		return true;
	}
}
