// The game's name hash, which the road geometry (tools/worldgen/roads.cpp) uses for street names; the loader's own is
// in its converter, which this mod does not link.
#include <cstdint>
#include <string_view>

namespace loader::convert
{
	uint32_t Joaat(std::string_view text)
	{
		uint32_t h = 0;
		for (const char c : text)
		{
			h += static_cast<uint8_t>(c >= 'A' && c <= 'Z' ? c + 32 : c);
			h += h << 10;
			h ^= h >> 6;
		}
		h += h << 3;
		h ^= h >> 11;
		h += h << 15;
		return h;
	}
}
