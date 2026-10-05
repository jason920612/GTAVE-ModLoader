#include "pattern.hpp"

#include <Windows.h>

#include <cstring>

namespace loader::pattern
{
	Pattern Pattern::Parse(std::string_view text)
	{
		Pattern p;
		size_t i = 0;
		while (i < text.size())
		{
			if (text[i] == ' ')
			{
				++i;
				continue;
			}
			if (text[i] == '?')
			{
				p.bytes.push_back(-1);
				i += (i + 1 < text.size() && text[i + 1] == '?') ? 2 : 1;
				continue;
			}
			auto hex = [](char c) { return c <= '9' ? c - '0' : (c | 0x20) - 'a' + 10; };
			p.bytes.push_back(static_cast<int16_t>(hex(text[i]) << 4 | hex(text[i + 1])));
			i += 2;
		}
		return p;
	}

	Module Module::Main()
	{
		Module m;
		m.base = reinterpret_cast<uintptr_t>(GetModuleHandleW(nullptr));
		auto dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(m.base);
		auto nt = reinterpret_cast<const IMAGE_NT_HEADERS*>(m.base + dos->e_lfanew);
		m.size = nt->OptionalHeader.SizeOfImage;

		auto section = IMAGE_FIRST_SECTION(nt);
		for (WORD i = 0; i < nt->FileHeader.NumberOfSections; ++i, ++section)
		{
			if (section->Characteristics & IMAGE_SCN_MEM_EXECUTE)
			{
				m.text = {reinterpret_cast<const uint8_t*>(m.base + section->VirtualAddress), section->Misc.VirtualSize};
				break;
			}
		}
		return m;
	}

	namespace
	{
		// Scans with memchr on the first concrete byte, then verifies the rest.
		template<class OnMatch>
		void Scan(std::span<const uint8_t> region, const Pattern& pattern, OnMatch&& onMatch)
		{
			const auto& pat = pattern.bytes;
			if (pat.empty() || region.size() < pat.size())
				return;

			size_t anchor = 0;
			while (anchor < pat.size() && pat[anchor] < 0)
				++anchor;
			if (anchor == pat.size())
				return;

			const uint8_t* begin = region.data();
			const uint8_t* last = begin + region.size() - pat.size();
			const uint8_t* cur = begin + anchor;
			while (cur <= last + anchor)
			{
				cur = static_cast<const uint8_t*>(memchr(cur, pat[anchor], (last + anchor) - cur + 1));
				if (!cur)
					return;
				const uint8_t* start = cur - anchor;
				bool match = true;
				for (size_t i = 0; i < pat.size(); ++i)
				{
					if (pat[i] >= 0 && start[i] != pat[i])
					{
						match = false;
						break;
					}
				}
				if (match && !onMatch(reinterpret_cast<uintptr_t>(start)))
					return;
				++cur;
			}
		}
	}

	std::optional<uintptr_t> Find(std::span<const uint8_t> region, const Pattern& pattern)
	{
		std::optional<uintptr_t> result;
		Scan(region, pattern, [&](uintptr_t at) {
			result = at;
			return false;
		});
		return result;
	}

	size_t Count(std::span<const uint8_t> region, const Pattern& pattern, size_t limit)
	{
		size_t n = 0;
		Scan(region, pattern, [&](uintptr_t) { return ++n < limit; });
		return n;
	}
}
