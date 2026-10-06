#pragma once
#include <algorithm>
#include <optional>
#include <string>
#include <vector>

#include "resource.hpp"

namespace loader::convert
{
	// Working space: the legacy virtual block, the legacy physical block, then pages we append, addressed as
	// kVirtual + offset; legacy physical pointers (0x6...) resolve too. Relayout() then packs every page
	// into a new virtual-only block (Enhanced fragments have no physical block) and rewrites all pointers.
	class Block
	{
	public:
		Bytes data;

		void Init(const Resource& in)
		{
			virtualSize_ = in.virtualBlock.size();
			physicalSize_ = in.physicalBlock.size();
			data = in.virtualBlock;
			data.insert(data.end(), in.physicalBlock.begin(), in.physicalBlock.end());
			size_t at = 0;
			for (const uint32_t size : PageList(in.virtualFlags))
				chunks_.push_back({at, size}), at += size;
			at = virtualSize_;
			for (const uint32_t size : PageList(in.physicalFlags))
				chunks_.push_back({at, size}), at += size;
		}

		std::optional<size_t> Alloc(size_t size, size_t align = 16)
		{
			if (size > kAppendPage)
			{
				// A page of its own (e.g. texture pixels): power-of-two size, at least 0x2000.
				uint32_t page = 0x2000;
				while (page < size)
					page *= 2;
				if (page > (0x2000u << 15))
					return std::nullopt;
				const size_t at = data.size();
				chunks_.push_back({at, page});
				data.resize(at + page);
				free_ = 0;
				return at;
			}
			if (free_)
			{
				const size_t used = kAppendPage - free_;
				const size_t start = (used + align - 1) & ~(align - 1);
				if (start + size <= kAppendPage)
				{
					free_ = kAppendPage - start - size;
					return data.size() - kAppendPage + start;
				}
			}
			chunks_.push_back({data.size(), static_cast<uint32_t>(kAppendPage)});
			data.resize(data.size() + kAppendPage);
			free_ = kAppendPage - size;
			return data.size() - kAppendPage;
		}

		std::optional<size_t> Ptr(uint64_t p) const
		{
			if (p >= kVirtual && p < kVirtual + data.size())
				return static_cast<size_t>(p - kVirtual);
			if (p >= kPhysical && p < kPhysical + physicalSize_)
				return virtualSize_ + static_cast<size_t>(p - kPhysical);
			return std::nullopt;
		}

		uint8_t U8(size_t o) const { return o < data.size() ? data[o] : 0; }
		uint16_t U16(size_t o) const { return Get<uint16_t>(data, o); }
		uint32_t U32(size_t o) const { return Get<uint32_t>(data, o); }
		uint64_t U64(size_t o) const { return Get<uint64_t>(data, o); }

		// Packs every page (chunk) into pages of the game's size classes. The game allocates each page on
		// its own, so chunks are never split; the root object (offset 0) stays at offset 0.
		bool Relayout(Bytes& out, uint32_t& flags, uint32_t& pageCount, std::string& error) const
		{
			uint32_t largest = 0;
			for (const Chunk& c : chunks_)
				largest = std::max(largest, c.size);
			uint32_t base = 0x2000;
			while (base * 16 < largest)
				base *= 2;
			struct Page
			{
				uint32_t size;
				uint32_t used;
			};
			std::vector<Page> pages;
			std::vector<std::pair<size_t, uint32_t>> placed(chunks_.size()); // (page, offset in page)
			std::vector<size_t> order(chunks_.size());
			for (size_t i = 0; i < order.size(); ++i)
				order[i] = i;
			// The chunk at offset 0 first (into the first, largest page), then largest first.
			std::stable_sort(order.begin(), order.end(), [&](size_t x, size_t y) {
				if ((chunks_[x].at == 0) != (chunks_[y].at == 0))
					return chunks_[x].at == 0;
				return chunks_[x].size > chunks_[y].size;
			});
			for (;; base *= 2)
			{
				if (base > (0x2000u << 15))
				{
					error = "cannot lay out the pages";
					return false;
				}
				pages.clear();
				for (const size_t i : order)
				{
					const uint32_t size = chunks_[i].size;
					const uint32_t align = std::min<uint32_t>(size, 0x2000);
					bool done = false;
					for (size_t p = 0; p < pages.size() && !done; ++p)
					{
						const uint32_t start = (pages[p].used + align - 1) & ~(align - 1);
						if (start + size <= pages[p].size)
						{
							placed[i] = {p, start};
							pages[p].used = start + size;
							done = true;
						}
					}
					if (!done)
					{
						uint32_t pageSize = pages.empty() ? std::max(largest, base) : base;
						while (pageSize < size)
							pageSize *= 2;
						placed[i] = {pages.size(), 0};
						pages.push_back({pageSize, size});
					}
				}
				uint32_t counts[5] = {}; // x16, x8, x4, x2, x1
				bool fits = true;
				for (const Page& page : pages)
				{
					const uint32_t ratio = page.size / base;
					const int cls = ratio == 16 ? 0 : ratio == 8 ? 1 : ratio == 4 ? 2 : ratio == 2 ? 3 : ratio == 1 ? 4 : -1;
					if (cls < 0)
						fits = false;
					else
						++counts[cls];
				}
				if (fits && counts[0] <= 1 && counts[1] <= 3 && counts[2] <= 15 && counts[3] <= 63 && counts[4] <= 127)
				{
					uint32_t shift = 0;
					while ((0x2000u << shift) < base)
						++shift;
					flags = shift | (counts[0] << 4) | (counts[1] << 5) | (counts[2] << 7) | (counts[3] << 11) | (counts[4] << 17);
					break;
				}
			}
			// Pages in file order: largest class first (the first page is one of the largest).
			std::vector<size_t> pageOrder(pages.size());
			for (size_t i = 0; i < pageOrder.size(); ++i)
				pageOrder[i] = i;
			std::stable_sort(pageOrder.begin(), pageOrder.end(), [&](size_t x, size_t y) { return pages[x].size > pages[y].size; });
			std::vector<size_t> pageStart(pages.size());
			size_t at = 0;
			for (const size_t p : pageOrder)
				pageStart[p] = at, at += pages[p].size;
			pageCount = static_cast<uint32_t>(pages.size());
			out.assign(at, 0);
			std::vector<size_t> newStart(chunks_.size());
			for (size_t i = 0; i < chunks_.size(); ++i)
			{
				newStart[i] = pageStart[placed[i].first] + placed[i].second;
				std::copy(data.begin() + chunks_[i].at, data.begin() + chunks_[i].at + chunks_[i].size, out.begin() + newStart[i]);
				if (chunks_[i].at == 0 && newStart[i] != 0)
				{
					error = "root object moved";
					return false;
				}
			}
			// Rewrite every pointer-looking value.
			std::vector<size_t> byStart(chunks_.size());
			for (size_t i = 0; i < byStart.size(); ++i)
				byStart[i] = i;
			std::sort(byStart.begin(), byStart.end(), [&](size_t x, size_t y) { return chunks_[x].at < chunks_[y].at; });
			for (size_t o = 0; o + 8 <= out.size(); o += 8)
			{
				const auto old = Ptr(Get<uint64_t>(out, o));
				if (!old)
					continue;
				const auto it = std::upper_bound(byStart.begin(), byStart.end(), *old, [&](size_t v, size_t c) { return v < chunks_[c].at; });
				if (it == byStart.begin())
					continue;
				const size_t c = *(it - 1);
				if (*old >= chunks_[c].at + chunks_[c].size)
					continue;
				Put<uint64_t>(out, o, kVirtual + newStart[c] + (*old - chunks_[c].at));
			}
			return true;
		}

	private:
		static constexpr size_t kAppendPage = 0x10000;
		struct Chunk
		{
			size_t at;
			uint32_t size;
		};
		std::vector<Chunk> chunks_;
		size_t virtualSize_ = 0, physicalSize_ = 0;
		size_t free_ = 0;
	};
}
