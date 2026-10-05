#include "archive.hpp"

#include <algorithm>
#include <cstring>
#include <deque>
#include <format>
#include <map>

namespace loader::convert
{

	namespace
	{
		constexpr uint32_t kDirectoryMarker = 0x7FFFFF00;
		constexpr uint32_t kRscMagic = 0x37435352; // 'RSC7'

		std::string Lower(std::string s)
		{
			std::transform(s.begin(), s.end(), s.begin(), [](char c) { return static_cast<char>(c >= 'A' && c <= 'Z' ? c + 32 : c); });
			return s;
		}

		bool ReadAt(std::ifstream& in, uint64_t at, void* data, size_t size)
		{
			in.clear();
			in.seekg(static_cast<std::streamoff>(at));
			return static_cast<bool>(in.read(static_cast<char*>(data), static_cast<std::streamsize>(size)));
		}
	}

	bool Archive::Open(std::shared_ptr<std::ifstream> in, uint64_t offset, uint64_t size, const std::string& name,
	    const Decryptor* decrypt, std::string& error)
	{
		in_ = std::move(in);
		offset_ = offset;
		decrypt_ = decrypt;
		uint32_t header[4] = {};
		if (!ReadAt(*in_, offset, header, sizeof(header)) || header[0] != kRpfMagic)
		{
			error = "not an RPF7 archive";
			return false;
		}
		const uint32_t count = header[1], namesSize = header[2] & 0x0FFFFFFF, shift = (header[2] >> 28) & 7;
		const size_t tocSize = static_cast<size_t>(count) * 16 + namesSize;
		if (!count || tocSize > (64u << 20))
		{
			error = "corrupt archive header";
			return false;
		}
		Bytes toc(tocSize);
		if (!ReadAt(*in_, offset + 16, toc.data(), toc.size()))
		{
			error = "truncated archive";
			return false;
		}
		if (header[3] == kEncryptionNg)
		{
			if (!decrypt_)
			{
				error = "encrypted archive";
				return false;
			}
			encrypted_ = true;
			(*decrypt_)((Joaat(Lower(name)) + static_cast<uint32_t>(size)) % 101, toc.data(), static_cast<uint32_t>(toc.size()));
		}
		else if (header[3] != kEncryptionOpen && header[3] != 0)
		{
			error = std::format("unsupported encryption {:#x}", header[3]);
			return false;
		}
		if (Get<uint32_t>(toc, 4) != kDirectoryMarker)
		{
			error = encrypted_ ? "cannot decrypt the archive" : "corrupt archive";
			return false;
		}

		const char* names = reinterpret_cast<const char*>(toc.data() + static_cast<size_t>(count) * 16);
		const auto nameAt = [&](uint32_t off) {
			const size_t o = static_cast<size_t>(off) << shift;
			return o < namesSize ? std::string(names + o, strnlen(names + o, namesSize - o)) : std::string();
		};
		nodes_.resize(count);
		for (uint32_t i = 0; i < count; ++i)
		{
			const size_t e = static_cast<size_t>(i) * 16;
			const uint64_t q = Get<uint64_t>(toc, e);
			const uint32_t a = Get<uint32_t>(toc, e + 8), b = Get<uint32_t>(toc, e + 12);
			Node& n = nodes_[i];
			if (static_cast<uint32_t>(q >> 32) == kDirectoryMarker)
			{
				n.directory = true;
				n.name = nameAt(static_cast<uint32_t>(q));
				for (uint32_t c = a; c < a + b && c < count; ++c)
					n.children.push_back(c);
				continue;
			}
			n.name = nameAt(static_cast<uint32_t>(q & 0xFFFF));
			n.stored = static_cast<uint32_t>((q >> 16) & 0xFFFFFF);
			n.offset = ((q >> 40) & 0x7FFFFF) * 512;
			n.resource = (q >> 63) != 0;
			if (n.resource)
			{
				n.flags[0] = a;
				n.flags[1] = b;
				if (n.stored == 0xFFFFFF)
				{
					error = std::format("{}: resource larger than 16 MB (not supported yet)", n.name);
					return false;
				}
			}
			else
			{
				n.size = a;
				n.encrypted = encrypted_ && b != 0;
			}
		}
		return true;
	}

	bool Archive::ReadFile(uint32_t index, Bytes& out, std::string& error) const
	{
		const Node& n = nodes_.at(index);
		Bytes raw(static_cast<size_t>(n.DiskSize()));
		if (!ReadAt(*in_, offset_ + n.offset, raw.data(), raw.size()))
		{
			error = std::format("{}: cannot read", n.name);
			return false;
		}
		if (n.resource)
		{
			if (raw.size() >= 16 && Get<uint32_t>(raw, 0) == kRscMagic)
			{
				out = std::move(raw); // tools such as OpenIV keep the RSC7 header in the archive
				return true;
			}
			// The game's own archives keep a 16-byte header that is not "RSC7" (the flags are in the entry)
			// followed by the plain deflate stream: rebuild a normal resource file.
			const size_t expected = static_cast<size_t>(BlockSize(n.flags[0])) + BlockSize(n.flags[1]);
			if (raw.size() > 16 && InflatedSize(raw.data() + 16, raw.size() - 16, std::min<size_t>(expected, 0x1000)))
			{
				Put<uint32_t>(raw, 0, kRscMagic);
				Put<uint32_t>(raw, 4, ((n.flags[0] >> 28) << 4) | (n.flags[1] >> 28));
				Put<uint32_t>(raw, 8, n.flags[0]);
				Put<uint32_t>(raw, 12, n.flags[1]);
				out = std::move(raw);
				return true;
			}
			error = std::format("{}: cannot read resource data", n.name);
			return false;
		}
		if (n.encrypted)
			(*decrypt_)((Joaat(Lower(n.name)) + n.size) % 101, raw.data(), static_cast<uint32_t>(raw.size()));
		if (!n.stored)
		{
			out = std::move(raw);
			return true;
		}
		out.assign(n.size, 0);
		if (!Inflate(raw.data(), raw.size(), out))
		{
			error = std::format("{}: cannot decompress", n.name);
			return false;
		}
		return true;
	}

	bool Archive::OpenNested(uint32_t index, Archive& out, std::string& error) const
	{
		const Node& n = nodes_.at(index);
		if (n.directory || n.resource || n.stored)
		{
			error = std::format("{}: not a stored archive", n.name);
			return false;
		}
		return out.Open(in_, offset_ + n.offset, n.size, n.name, decrypt_, error);
	}

	bool BuildArchive(const WriteNode& root, Bytes& out, std::string& error)
	{
		// Breadth-first so each directory's children are contiguous; children sorted by name.
		struct Entry
		{
			const WriteNode* node;
			std::string name;
			uint32_t first = 0, count = 0;
			uint32_t nameOffset = 0;
			uint64_t block = 0;
			Bytes nested; // built nested archive
		};
		std::deque<Entry> entries;
		entries.push_back({&root, ""});
		for (size_t i = 0; i < entries.size(); ++i)
		{
			if (entries[i].node->kind != WriteNode::Kind::Directory)
				continue;
			std::vector<const WriteNode*> children;
			for (const WriteNode& c : entries[i].node->children)
				children.push_back(&c);
			std::sort(children.begin(), children.end(), [](const WriteNode* a, const WriteNode* b) { return Lower(a->name) < Lower(b->name); });
			entries[i].first = static_cast<uint32_t>(entries.size());
			entries[i].count = static_cast<uint32_t>(children.size());
			for (const WriteNode* c : children)
				entries.push_back({c, Lower(c->name)});
		}
		if (entries.size() > 0xFFFF)
		{
			error = "too many files in one archive";
			return false;
		}
		for (Entry& e : entries)
			if (e.node->kind == WriteNode::Kind::Archive)
			{
				WriteNode dir = *e.node;
				dir.kind = WriteNode::Kind::Directory;
				if (!BuildArchive(dir, e.nested, error))
					return false;
			}

		// Names: offset 0 is the root's empty name. File entries store the offset in 16 bits, shifted right by
		// the name shift (names are then aligned to 1 << shift).
		std::map<std::string, uint32_t> nameOffsets{{"", 0}};
		uint32_t shift = 0;
		Bytes names;
		for (;; ++shift)
		{
			names.assign(1, 0);
			nameOffsets = {{"", 0}};
			const uint32_t align = 1u << shift;
			for (const Entry& e : entries)
				if (!nameOffsets.contains(e.name))
				{
					while (names.size() % align)
						names.push_back(0);
					nameOffsets[e.name] = static_cast<uint32_t>(names.size());
					names.insert(names.end(), e.name.begin(), e.name.end());
					names.push_back(0);
				}
			if ((names.size() >> shift) < 0x10000)
				break;
			if (shift == 7)
			{
				error = "names table too large";
				return false;
			}
		}
		while (names.size() % 16)
			names.push_back(0);

		const size_t toc = 16 + entries.size() * 16 + names.size();
		uint64_t offset = (toc + 511) / 512 * 512;
		for (Entry& e : entries)
		{
			e.nameOffset = nameOffsets[e.name] >> shift;
			if (e.node->kind == WriteNode::Kind::Directory)
				continue;
			const size_t size = e.node->kind == WriteNode::Kind::Archive ? e.nested.size() : e.node->data.size();
			e.block = offset / 512;
			offset += (size + 511) / 512 * 512;
		}
		if (offset / 512 >= (1u << 23))
		{
			error = "archive larger than 4 GB";
			return false;
		}

		out.assign(static_cast<size_t>(offset), 0);
		Put<uint32_t>(out, 0, kRpfMagic);
		Put<uint32_t>(out, 4, static_cast<uint32_t>(entries.size()));
		Put<uint32_t>(out, 8, static_cast<uint32_t>(names.size()) | (shift << 28));
		Put<uint32_t>(out, 12, kEncryptionOpen);
		for (size_t i = 0; i < entries.size(); ++i)
		{
			const Entry& e = entries[i];
			const size_t at = 16 + i * 16;
			switch (e.node->kind)
			{
			case WriteNode::Kind::Directory:
				Put<uint32_t>(out, at, e.nameOffset);
				Put<uint32_t>(out, at + 4, kDirectoryMarker);
				Put<uint32_t>(out, at + 8, e.first);
				Put<uint32_t>(out, at + 12, e.count);
				break;
			case WriteNode::Kind::Resource:
			{
				const Bytes& d = e.node->data;
				if (d.size() >= 0xFFFFFF || d.size() < 16)
				{
					error = std::format("{}: resource of {} bytes cannot be stored", e.name, d.size());
					return false;
				}
				Put<uint64_t>(out, at, e.nameOffset | (static_cast<uint64_t>(d.size()) << 16) | (e.block << 40) | (1ull << 63));
				Put<uint32_t>(out, at + 8, Get<uint32_t>(d, 8));
				Put<uint32_t>(out, at + 12, Get<uint32_t>(d, 12));
				std::copy(d.begin(), d.end(), out.begin() + static_cast<size_t>(e.block * 512));
				break;
			}
			case WriteNode::Kind::File:
			case WriteNode::Kind::Archive:
			{
				const Bytes& d = e.node->kind == WriteNode::Kind::Archive ? e.nested : e.node->data;
				Put<uint64_t>(out, at, e.nameOffset | (e.block << 40));
				Put<uint32_t>(out, at + 8, static_cast<uint32_t>(d.size()));
				std::copy(d.begin(), d.end(), out.begin() + static_cast<size_t>(e.block * 512));
				break;
			}
			}
		}
		std::copy(names.begin(), names.end(), out.begin() + 16 + static_cast<std::ptrdiff_t>(entries.size()) * 16);
		return true;
	}
}
