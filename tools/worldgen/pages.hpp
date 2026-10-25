#pragma once
#include <cstdint>
#include <vector>

// Laying out a resource's virtual block in pages of 16 KB: an object never crosses a page (the game places each page
// on its own), small objects share pages, a big one (up to a page) can take a page of its own.
namespace worldgen
{
	struct Pages
	{
		static constexpr uint32_t kSize = 0x4000;
		std::vector<uint32_t> used; // bytes used in each page

		// Somewhere with room (the first page that has it), else a new page. Returns the offset in the block.
		uint32_t Alloc(uint32_t size, uint32_t align = 16)
		{
			for (size_t p = 0; p < used.size(); ++p)
			{
				const uint32_t at = (used[p] + align - 1) & ~(align - 1);
				if (at + size <= kSize)
				{
					used[p] = at + size;
					return static_cast<uint32_t>(p * kSize + at);
				}
			}
			return Page(size);
		}

		// A new page of its own.
		uint32_t Page(uint32_t size)
		{
			used.push_back(size);
			return static_cast<uint32_t>((used.size() - 1) * kSize);
		}

		// The page map (16 bytes and a pointer per page, which the game fills in), placed last: returns its offset.
		uint32_t PageMap()
		{
			const uint32_t pages = static_cast<uint32_t>(used.size());
			const uint32_t at = Alloc(0x10 + 8 * pages);
			if (used.size() != pages)
				used.back() += 8; // (it took a page of its own: one more pointer, which still fits)
			return at;
		}

		uint32_t Total() const
		{
			uint32_t t = 0;
			for (const uint32_t u : used)
				t += u;
			return t;
		}
	};
}
