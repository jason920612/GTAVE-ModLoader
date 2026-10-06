#include "peds.hpp"

#include <algorithm>
#include <array>
#include <format>
#include <optional>
#include <set>

namespace loader::convert
{
	namespace
	{
		constexpr std::array<const char*, 12> kComponents = {"head", "berd", "hair", "uppr", "lowr", "hand", "feet", "teef", "accs", "task", "decl", "jbib"};

		// The variation info holds 4 flags, then u8 availComp[12] (index into the component data, 0xFF = none),
		// then the component data array whose count (+0x18) equals the number of available components. It is
		// found by that shape and by a pointer to it (research/phase0.md §19).
		std::optional<size_t> FindVariationInfo(const Bytes& b)
		{
			std::set<uint64_t> pointers;
			for (size_t o = 0; o + 8 <= b.size(); o += 8)
				if (const uint64_t v = Get<uint64_t>(b, o); v >= kVirtual && v < kVirtual + b.size())
					pointers.insert(v - kVirtual);
			for (const uint64_t s : pointers)
			{
				if (s + 0x1C > b.size())
					continue;
				std::set<uint8_t> seen;
				int available = 0;
				bool ok = true;
				for (int c = 0; c < 12 && ok; ++c)
				{
					const uint8_t v = b[s + 4 + c];
					if (v == 0xFF)
						continue;
					ok = v < 12 && seen.insert(v).second;
					++available;
				}
				if (ok && available && Get<uint16_t>(b, s + 0x18) == available)
					return static_cast<size_t>(s);
			}
			return std::nullopt;
		}
	}

	bool DropMissingComponents(Bytes& ymt, const Bytes& dictionary, const std::string& name, std::vector<std::string>& warnings)
	{
		Resource meta, dict;
		std::string error;
		if (!Read(ymt, meta, error) || !Read(dictionary, dict, error))
			return false;
		// Drawable dictionary: +0x20 name hashes, +0x28 count.
		const Bytes& d = dict.virtualBlock;
		const uint64_t hashesPtr = Get<uint64_t>(d, 0x20);
		const uint16_t count = Get<uint16_t>(d, 0x28);
		if (hashesPtr < kVirtual || hashesPtr - kVirtual + 4ull * count > d.size())
			return false;
		std::set<uint32_t> drawables;
		for (uint16_t i = 0; i < count; ++i)
			drawables.insert(Get<uint32_t>(d, static_cast<size_t>(hashesPtr - kVirtual) + 4 * i));

		const auto info = FindVariationInfo(meta.virtualBlock);
		if (!info)
			return false;
		bool changed = false;
		std::string missing;
		for (int c = 0; c < 12; ++c)
		{
			uint8_t& slot = meta.virtualBlock[*info + 4 + c];
			if (slot == 0xFF)
				continue;
			bool any = false;
			for (int i = 0; i < 64 && !any; ++i)
				for (const char suffix : {'u', 'r', 'm', 'f'})
					any |= drawables.contains(Joaat(std::format("{}_{:03}_{}", kComponents[c], i, suffix)));
			if (any)
				continue;
			slot = 0xFF;
			changed = true;
			missing += (missing.empty() ? "" : ", ") + std::string(kComponents[c]);
		}
		if (!changed)
			return false;
		Bytes out;
		if (!Write(meta, out, error))
			return false;
		ymt = std::move(out);
		warnings.push_back(std::format("{}：角色宣告的部位沒有對應的模型（{}），已略過這些部位", name, missing));
		return true;
	}
}
