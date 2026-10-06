#include "oiv.hpp"

#include <algorithm>
#include <format>
#include <fstream>
#include <string_view>

namespace loader::convert
{
	namespace
	{
		struct ZipEntry
		{
			std::string name; // forward slashes
			uint16_t method = 0;
			uint32_t packed = 0, size = 0, local = 0;
		};

		std::string Lower(std::string s)
		{
			for (auto& c : s)
				if (c >= 'A' && c <= 'Z')
					c = static_cast<char>(c - 'A' + 'a');
			return s;
		}

		std::string Slashes(std::string s)
		{
			for (auto& c : s)
				if (c == '\\')
					c = '/';
			return s;
		}

		template<class T>
		T At(const Bytes& b, size_t offset)
		{
			return Get<T>(b, offset);
		}

		bool ReadAt(std::ifstream& in, uint64_t offset, size_t size, Bytes& out)
		{
			out.resize(size);
			in.clear();
			in.seekg(static_cast<std::streamoff>(offset));
			return size == 0 || static_cast<bool>(in.read(reinterpret_cast<char*>(out.data()), static_cast<std::streamsize>(size)));
		}

		bool Entries(std::ifstream& in, std::vector<ZipEntry>& out, std::string& error)
		{
			in.seekg(0, std::ios::end);
			const uint64_t fileSize = static_cast<uint64_t>(in.tellg());
			const size_t tail = static_cast<size_t>(std::min<uint64_t>(fileSize, 0x10000 + 22));
			Bytes end;
			if (!ReadAt(in, fileSize - tail, tail, end))
			{
				error = "cannot read the file";
				return false;
			}
			size_t eocd = std::string::npos;
			for (size_t i = tail >= 22 ? tail - 22 : 0; i != static_cast<size_t>(-1); --i)
				if (At<uint32_t>(end, i) == 0x06054b50)
				{
					eocd = i;
					break;
				}
			if (eocd == std::string::npos)
			{
				error = "not a zip archive";
				return false;
			}
			const uint16_t count = At<uint16_t>(end, eocd + 10);
			const uint32_t dirSize = At<uint32_t>(end, eocd + 12), dirOffset = At<uint32_t>(end, eocd + 16);
			if (dirOffset == 0xFFFFFFFF || count == 0xFFFF)
			{
				error = "zip64 packages are not supported";
				return false;
			}
			Bytes dir;
			if (!ReadAt(in, dirOffset, dirSize, dir))
			{
				error = "cannot read the zip directory";
				return false;
			}
			size_t p = 0;
			for (uint16_t i = 0; i < count; ++i)
			{
				if (p + 46 > dir.size() || At<uint32_t>(dir, p) != 0x02014b50)
				{
					error = "damaged zip directory";
					return false;
				}
				ZipEntry e;
				e.method = At<uint16_t>(dir, p + 10);
				e.packed = At<uint32_t>(dir, p + 20);
				e.size = At<uint32_t>(dir, p + 24);
				const uint16_t nameLength = At<uint16_t>(dir, p + 28), extra = At<uint16_t>(dir, p + 30), comment = At<uint16_t>(dir, p + 32);
				e.local = At<uint32_t>(dir, p + 42);
				if (p + 46 + nameLength > dir.size())
				{
					error = "damaged zip directory";
					return false;
				}
				e.name = Slashes(std::string(reinterpret_cast<const char*>(dir.data() + p + 46), nameLength));
				out.push_back(std::move(e));
				p += 46 + nameLength + extra + comment;
			}
			return true;
		}

		bool Extract(std::ifstream& in, const ZipEntry& e, Bytes& out, std::string& error)
		{
			Bytes header;
			if (!ReadAt(in, e.local, 30, header) || At<uint32_t>(header, 0) != 0x04034b50)
			{
				error = std::format("{}: damaged entry", e.name);
				return false;
			}
			const uint64_t data = uint64_t(e.local) + 30 + At<uint16_t>(header, 26) + At<uint16_t>(header, 28);
			Bytes packed;
			if (!ReadAt(in, data, e.packed, packed))
			{
				error = std::format("{}: cannot read", e.name);
				return false;
			}
			if (e.method == 0)
			{
				out = std::move(packed);
				return true;
			}
			if (e.method != 8)
			{
				error = std::format("{}: compression method {} is not supported", e.name, e.method);
				return false;
			}
			out.assign(e.size, 0);
			if (!Inflate(packed.data(), packed.size(), out))
			{
				error = std::format("{}: cannot decompress", e.name);
				return false;
			}
			return true;
		}

		const ZipEntry* FindEntry(const std::vector<ZipEntry>& entries, const std::string& name)
		{
			const std::string wanted = Lower(Slashes(name));
			for (const auto& e : entries)
				if (Lower(e.name) == wanted)
					return &e;
			return nullptr;
		}

		std::string Attribute(std::string_view tag, std::string_view name)
		{
			const std::string key = std::string(name) + "=\"";
			const size_t at = tag.find(key);
			if (at == std::string_view::npos)
				return {};
			const size_t end = tag.find('"', at + key.size());
			return end == std::string_view::npos ? std::string() : std::string(tag.substr(at + key.size(), end - at - key.size()));
		}

		std::string Unescape(std::string s)
		{
			const std::pair<std::string_view, std::string_view> entities[] = {{"&lt;", "<"}, {"&gt;", ">"}, {"&quot;", "\""}, {"&apos;", "'"}, {"&amp;", "&"}};
			for (const auto& [from, to] : entities)
				for (size_t at = 0; (at = s.find(from, at)) != std::string::npos; at += to.size())
					s.replace(at, from.size(), to);
			return s;
		}

		std::string Trimmed(std::string_view s)
		{
			while (!s.empty() && (s.front() == ' ' || s.front() == '\t' || s.front() == '\r' || s.front() == '\n'))
				s.remove_prefix(1);
			while (!s.empty() && (s.back() == ' ' || s.back() == '\t' || s.back() == '\r' || s.back() == '\n'))
				s.remove_suffix(1);
			return std::string(s);
		}
	}

	bool ReadOiv(const std::filesystem::path& file, Oiv& out, std::string& error)
	{
		std::ifstream in(file, std::ios::binary);
		std::vector<ZipEntry> entries;
		if (!in || !Entries(in, entries, error))
			return false;
		const ZipEntry* assembly = FindEntry(entries, "assembly.xml");
		Bytes data;
		if (!assembly)
		{
			error = "assembly.xml is missing";
			return false;
		}
		if (!Extract(in, *assembly, data, error))
			return false;
		std::string xml(data.begin(), data.end());
		if (xml.starts_with("\xEF\xBB\xBF"))
			xml.erase(0, 3);
		if (const size_t a = xml.find("<name>"), b = xml.find("</name>"); a != std::string::npos && b != std::string::npos && b > a)
			out.title = Unescape(Trimmed(std::string_view(xml).substr(a + 6, b - a - 6)));
		const size_t content = xml.find("<content>");
		if (content == std::string::npos)
		{
			error = "assembly.xml has no <content>";
			return false;
		}
		for (size_t at = content; (at = xml.find('<', at + 1)) != std::string::npos;)
		{
			const size_t close = xml.find('>', at);
			if (close == std::string::npos)
				break;
			const std::string_view tag(xml.data() + at, close - at + 1);
			if (tag.starts_with("<add ") || tag.starts_with("<add\t"))
			{
				const std::string source = Attribute(tag, "source");
				if (source.empty())
					continue; // an edit inside <text> / <xml>, handled with its parent
				const size_t end = xml.find("</add>", close);
				if (end == std::string::npos)
					break;
				OivFile f;
				f.source = "content/" + Slashes(Unescape(source));
				f.target = Slashes(Unescape(Trimmed(std::string_view(xml).substr(close + 1, end - close - 1))));
				const size_t slash = f.target.rfind('/');
				f.name = Lower(slash == std::string::npos ? f.target : f.target.substr(slash + 1));
				if (f.name == "dlc.rpf" && slash != std::string::npos)
				{
					const size_t parent = f.target.rfind('/', slash - 1);
					f.dlcPack = true;
					f.packName = Lower(f.target.substr(parent == std::string::npos ? 0 : parent + 1, slash - (parent == std::string::npos ? 0 : parent + 1)));
				}
				out.files.push_back(std::move(f));
				at = end;
			}
			else if (tag.starts_with("<text ") || tag.starts_with("<xml "))
			{
				const std::string path = Slashes(Unescape(Attribute(tag, "path")));
				if (!Lower(path).ends_with("dlclist.xml"))
					out.warnings.push_back(std::format("安裝包中對 {} 的修改無法套用，已略過", path));
			}
			else if (tag.starts_with("<delete"))
			{
				const size_t end = xml.find("</delete>", close);
				const std::string path = end == std::string::npos ? std::string() : Trimmed(std::string_view(xml).substr(close + 1, end - close - 1));
				out.warnings.push_back(std::format("安裝包要求刪除 {}，已略過", Slashes(Unescape(path))));
			}
		}
		for (const OivFile& f : out.files)
			if (!FindEntry(entries, f.source))
				out.warnings.push_back(std::format("安裝包缺少 {}", f.source));
		return true;
	}

	bool ReadOivFile(const std::filesystem::path& file, const std::string& source, Bytes& out, std::string& error)
	{
		std::ifstream in(file, std::ios::binary);
		std::vector<ZipEntry> entries;
		if (!in || !Entries(in, entries, error))
			return false;
		const ZipEntry* e = FindEntry(entries, source);
		if (!e)
		{
			error = source + " is missing";
			return false;
		}
		return Extract(in, *e, out, error);
	}
}
