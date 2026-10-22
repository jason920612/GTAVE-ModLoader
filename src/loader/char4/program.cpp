#include "program.hpp"

#include <algorithm>
#include <cstring>
#include <format>

namespace loader::char4
{
	namespace
	{
		constexpr uint32_t kGlobalPage = 0x4000; // slots in a page of global initial values (0x20000 bytes)

		uint64_t Rotl(uint64_t v, uint32_t n)
		{
			n &= 63;
			return n ? (v << n) | (v >> (64 - n)) : v;
		}
		uint64_t Rotr(uint64_t v, uint32_t n)
		{
			return Rotl(v, 64 - (n & 63));
		}

		// Offset in the virtual block of a resource pointer, or SIZE_MAX.
		size_t Offset(const Bytes& block, uint64_t pointer, size_t size)
		{
			if ((pointer >> 28) != 5)
				return SIZE_MAX;
			const size_t offset = pointer & 0x0FFFFFFF;
			return offset + size <= block.size() ? offset : SIZE_MAX;
		}

		// Pages of `size` bytes in total through a table of page pointers.
		bool ReadPages(const Bytes& v, uint64_t tablePointer, size_t size, size_t pageBytes, Bytes& out)
		{
			out.clear();
			const size_t pages = (size + pageBytes - 1) / pageBytes;
			const size_t table = Offset(v, tablePointer, pages * 8);
			if (pages && table == SIZE_MAX)
				return false;
			for (size_t i = 0; i < pages; ++i)
			{
				const size_t n = std::min(pageBytes, size - i * pageBytes);
				const size_t at = Offset(v, convert::Get<uint64_t>(v, table + 8 * i), n);
				if (at == SIZE_MAX)
					return false;
				out.insert(out.end(), v.begin() + at, v.begin() + at + n);
			}
			return true;
		}
	}

	uint32_t InstructionSize(const Bytes& code, uint32_t ip)
	{
		const uint8_t o = code[ip];
		if (o == 37 || (o >= 52 && o <= 62) || (o >= 64 && o <= 66) || (o >= 104 && o <= 107))
			return 2;
		if (o == 38 || o == 46 || (o >= 67 && o <= 92))
			return 3;
		if (o == 39 || o == 44 || (o >= 93 && o <= 100))
			return 4;
		if (o == 40 || o == 41)
			return 5;
		if (o == 45)
			return 5 + (ip + 4 < code.size() ? code[ip + 4] : 0);
		if (o == 101)
			return 2 + 6 * (ip + 1 < code.size() ? code[ip + 1] : 0);
		return 1;
	}

	std::vector<Instruction> Decode(const Bytes& code, uint32_t start, uint32_t end)
	{
		end = std::min<uint32_t>(end, static_cast<uint32_t>(code.size()));
		std::vector<Instruction> out;
		for (uint32_t ip = start; ip < end;)
		{
			const uint32_t n = InstructionSize(code, ip);
			out.push_back({ip, code[ip], n});
			ip += n;
		}
		return out;
	}

	std::vector<SwitchCase> SwitchCases(const Bytes& code, uint32_t at)
	{
		std::vector<SwitchCase> cases;
		const uint8_t n = code[at + 1];
		for (uint32_t k = 0; k < n; ++k)
		{
			const uint32_t entry = at + 2 + 6 * k;
			cases.push_back({S32(code, entry), entry + 6 + S16(code, entry + 4)});
		}
		return cases;
	}

	bool Program::Read(const Bytes& file, std::string& error)
	{
		convert::Resource res;
		if (!convert::Read(file, res, error))
			return false;
		const Bytes& v = res.virtualBlock;
		if (v.size() < 0x80)
		{
			error = "not a script";
			return false;
		}
		version = res.version;
		virtualFlags = res.virtualFlags;
		physicalFlags = res.physicalFlags;
		header.assign(v.begin(), v.begin() + 0x80);
		const auto q = [&](size_t o) { return convert::Get<uint64_t>(v, o); };
		const auto u = [&](size_t o) { return convert::Get<uint32_t>(v, o); };
		const uint32_t codeSize = u(0x1C);
		if (!ReadPages(v, q(0x10), codeSize, kCodePage, code) || !ReadPages(v, q(0x68), u(0x70), kCodePage, strings))
		{
			error = "damaged script (pages)";
			return false;
		}
		// Natives are stored rotated right by (code size + index).
		const uint32_t nativeCount = u(0x2C);
		if (nativeCount)
		{
			const size_t at = Offset(v, q(0x40), size_t{nativeCount} * 8);
			if (at == SIZE_MAX)
			{
				error = "damaged script (natives)";
				return false;
			}
			for (uint32_t i = 0; i < nativeCount; ++i)
				natives.push_back(Rotl(convert::Get<uint64_t>(v, at + 8 * i), codeSize + i));
		}
		const uint32_t staticCount = u(0x24);
		if (staticCount)
		{
			const size_t at = Offset(v, q(0x30), size_t{staticCount} * 8);
			if (at == SIZE_MAX)
			{
				error = "damaged script (statics)";
				return false;
			}
			for (uint32_t i = 0; i < staticCount; ++i)
				statics.push_back(convert::Get<uint64_t>(v, at + 8 * i));
		}
		globalsWord = u(0x28);
		const uint32_t globalCount = globalsWord & 0x3FFFF;
		if (globalCount && q(0x38))
		{
			Bytes raw;
			if (!ReadPages(v, q(0x38), size_t{globalCount} * 8, size_t{kGlobalPage} * 8, raw))
			{
				error = "damaged script (globals)";
				return false;
			}
			globals.resize(globalCount);
			std::memcpy(globals.data(), raw.data(), raw.size());
		}
		const size_t nameAt = Offset(v, q(0x60), 1);
		if (nameAt == SIZE_MAX)
		{
			error = "damaged script (name)";
			return false;
		}
		for (size_t i = nameAt; i < v.size() && v[i]; ++i)
			name += static_cast<char>(v[i]);
		return true;
	}

	bool Program::Write(Bytes& out, std::string& error) const
	{
		const auto split = [](const uint8_t* data, size_t size, size_t page) {
			std::vector<Bytes> pages;
			for (size_t i = 0; i < size; i += page)
				pages.emplace_back(data + i, data + std::min(size, i + page));
			return pages;
		};
		const auto codePages = split(code.data(), code.size(), kCodePage);
		const auto strPages = split(strings.data(), strings.size(), kCodePage);
		const auto globPages = split(reinterpret_cast<const uint8_t*>(globals.data()), globals.size() * 8, size_t{kGlobalPage} * 8);
		Bytes nativeBytes(natives.size() * 8), staticBytes(statics.size() * 8);
		for (size_t i = 0; i < natives.size(); ++i)
		{
			const uint64_t stored = Rotr(natives[i], static_cast<uint32_t>(code.size() + i));
			std::memcpy(nativeBytes.data() + 8 * i, &stored, 8);
		}
		if (!statics.empty())
			std::memcpy(staticBytes.data(), statics.data(), staticBytes.size());

		// The root (program header, page map, page tables, name) is the block's first bytes. The page map gets the
		// address of every page of the block when the game loads it: 0x10 bytes and a slot per page plus one; room for
		// the most pages a block can have (a smaller map let the game overwrite the code page table, §55.6).
		constexpr size_t kPageMapAt = 0x80, kPageMapSize = 0x10 + 8 * (127 + 1);
		const size_t codeTableAt = kPageMapAt + kPageMapSize;
		const size_t strTableAt = codeTableAt + 8 * codePages.size();
		const size_t globTableAt = strTableAt + 8 * strPages.size();
		const size_t nameAt = globTableAt + 8 * globPages.size();
		const size_t emptyAt = (nameAt + name.size() + 1 + 15) & ~size_t{15}; // where empty statics and natives point
		const size_t rootSize = emptyAt + 16;

		enum class Kind { Root, Code, Str, Glob, Statics, Natives };
		struct Item
		{
			Kind kind;
			size_t index;
			const Bytes* data;
			size_t size;
			size_t at = 0;
		};
		std::vector<Item> items{{Kind::Root, 0, nullptr, rootSize}};
		for (size_t i = 0; i < codePages.size(); ++i)
			items.push_back({Kind::Code, i, &codePages[i], codePages[i].size()});
		for (size_t i = 0; i < strPages.size(); ++i)
			items.push_back({Kind::Str, i, &strPages[i], strPages[i].size()});
		for (size_t i = 0; i < globPages.size(); ++i)
			items.push_back({Kind::Glob, i, &globPages[i], globPages[i].size()});
		if (!staticBytes.empty())
			items.push_back({Kind::Statics, 0, &staticBytes, staticBytes.size()});
		if (!nativeBytes.empty())
			items.push_back({Kind::Natives, 0, &nativeBytes, nativeBytes.size()});
		std::stable_sort(items.begin() + 1, items.end(), [](const Item& a, const Item& b) { return a.size > b.size; });

		size_t largest = 0;
		for (const Item& it : items)
			largest = std::max(largest, it.size);
		uint32_t shift = 0;
		while ((size_t{0x2000} << shift) < largest)
			++shift;
		// A full code or string page starts on a 0x4000 boundary, as in the game's files: the interpreter runs from one
		// code page into the next by address, so a misplaced page runs into other data.
		const auto align = [](const Item& it) -> size_t {
			return (it.kind == Kind::Code || it.kind == Kind::Str) && it.size == kCodePage ? kCodePage : 16;
		};
		std::vector<size_t> used;
		size_t page = 0;
		for (;; ++shift)
		{
			if (shift > 15)
			{
				error = "script too large";
				return false;
			}
			page = size_t{0x2000} << shift;
			used.clear();
			for (Item& it : items)
			{
				const size_t a = align(it);
				bool placed = false;
				for (size_t i = 0; i < used.size() && !placed; ++i)
				{
					const size_t pos = (used[i] + a - 1) & ~(a - 1);
					if (pos + it.size <= page)
					{
						it.at = i * page + pos;
						used[i] = (pos + it.size + 15) & ~size_t{15};
						placed = true;
					}
				}
				if (!placed)
				{
					it.at = used.size() * page;
					used.push_back((it.size + 15) & ~size_t{15});
				}
			}
			if (used.size() <= 127)
				break;
		}
		const uint32_t flags = (virtualFlags & 0xF0000000) | shift | static_cast<uint32_t>(used.size() << 17);
		if (convert::BlockSize(flags) != used.size() * page)
		{
			error = "block layout";
			return false;
		}

		convert::Resource res;
		res.version = version;
		res.virtualFlags = flags;
		res.physicalFlags = physicalFlags;
		res.physicalBlock.resize(convert::BlockSize(physicalFlags));
		Bytes& block = res.virtualBlock;
		block.assign(convert::BlockSize(flags), 0);
		size_t staticsAt = emptyAt, nativesAt = emptyAt;
		std::vector<size_t> codeAt(codePages.size()), strAt(strPages.size()), globAt(globPages.size());
		for (const Item& it : items)
		{
			if (it.data)
				std::memcpy(block.data() + it.at, it.data->data(), it.size);
			switch (it.kind)
			{
			case Kind::Code: codeAt[it.index] = it.at; break;
			case Kind::Str: strAt[it.index] = it.at; break;
			case Kind::Glob: globAt[it.index] = it.at; break;
			case Kind::Statics: staticsAt = it.at; break;
			case Kind::Natives: nativesAt = it.at; break;
			case Kind::Root: break;
			}
		}
		const auto vp = [](size_t offset) { return uint64_t{convert::kVirtual} + offset; };
		std::memcpy(block.data(), header.data(), 0x80);
		convert::Put<uint64_t>(block, 0x08, vp(kPageMapAt));
		convert::Put<uint64_t>(block, 0x10, vp(codeTableAt));
		convert::Put<uint32_t>(block, 0x1C, static_cast<uint32_t>(code.size()));
		convert::Put<uint32_t>(block, 0x24, static_cast<uint32_t>(statics.size()));
		convert::Put<uint32_t>(block, 0x28, globalsWord);
		convert::Put<uint32_t>(block, 0x2C, static_cast<uint32_t>(natives.size()));
		convert::Put<uint64_t>(block, 0x30, vp(staticsAt));
		convert::Put<uint64_t>(block, 0x38, globPages.empty() ? 0 : vp(globTableAt));
		convert::Put<uint64_t>(block, 0x40, vp(nativesAt));
		convert::Put<uint64_t>(block, 0x60, vp(nameAt));
		convert::Put<uint64_t>(block, 0x68, vp(strTableAt));
		convert::Put<uint32_t>(block, 0x70, static_cast<uint32_t>(strings.size()));
		std::memset(block.data() + kPageMapAt, 0, kPageMapSize);
		block[kPageMapAt + 8] = static_cast<uint8_t>(used.size());
		for (size_t i = 0; i < codeAt.size(); ++i)
			convert::Put<uint64_t>(block, codeTableAt + 8 * i, vp(codeAt[i]));
		for (size_t i = 0; i < strAt.size(); ++i)
			convert::Put<uint64_t>(block, strTableAt + 8 * i, vp(strAt[i]));
		for (size_t i = 0; i < globAt.size(); ++i)
			convert::Put<uint64_t>(block, globTableAt + 8 * i, vp(globAt[i]));
		std::memcpy(block.data() + nameAt, name.c_str(), name.size() + 1);
		return convert::Write(res, out, error);
	}

	uint16_t Program::NativeIndex(uint64_t hash)
	{
		const auto it = std::find(natives.begin(), natives.end(), hash);
		if (it != natives.end())
			return static_cast<uint16_t>(it - natives.begin());
		natives.push_back(hash);
		return static_cast<uint16_t>(natives.size() - 1);
	}

	Functions::Functions(const std::vector<Instruction>& ins, uint32_t size) : codeSize(size)
	{
		for (const Instruction& i : ins)
			if (i.op == op::Enter)
				starts.push_back(i.at);
	}

	uint32_t Functions::Owner(uint32_t at) const
	{
		const auto it = std::upper_bound(starts.begin(), starts.end(), at);
		return it == starts.begin() ? 0 : *std::prev(it);
	}

	uint32_t Functions::End(uint32_t start) const
	{
		const auto it = std::upper_bound(starts.begin(), starts.end(), start);
		return it == starts.end() ? codeSize : *it;
	}
}
