#pragma once
#include <cstdint>
#include <cstring>
#include <string>
#include <string_view>
#include <vector>

// Resource container helpers shared by the legacy -> Enhanced converters (research/phase0.md §12-16).
namespace loader::convert
{
	using Bytes = std::vector<uint8_t>;

	// Pointers inside a resource: virtual block 0x50000000 + offset, physical block 0x60000000 + offset.
	constexpr uint32_t kVirtual = 0x50000000;
	constexpr uint32_t kPhysical = 0x60000000;

	// Block flags (the game's own encoding, GTA5_Enhanced.exe +0x1579E0): base page 0x2000 << (f & 0xF),
	// page counts per size class, fractional pages in bits 24-27, version nibble in bits 28-31.
	uint32_t BlockSize(uint32_t flags);
	uint32_t PageCount(uint32_t flags);
	std::vector<uint32_t> PageList(uint32_t flags); // page sizes in file order, largest class first

	// RAGE's case-insensitive one-at-a-time hash.
	uint32_t Joaat(std::string_view text);

	// Raw deflate through the zlib1.dll that ships with the game. `out` must have the expected size.
	bool Inflate(const uint8_t* data, size_t size, Bytes& out);
	bool Deflate(const Bytes& data, Bytes& out);
	// How many bytes a raw deflate stream decompresses to (up to `limit`); 0 if it is not valid deflate.
	size_t InflatedSize(const uint8_t* data, size_t size, size_t limit);

	// A resource file: RSC7 header {magic, version, virtual flags, physical flags} + raw deflate of the
	// virtual block followed by the physical block.
	struct Resource
	{
		uint32_t version = 0;
		uint32_t virtualFlags = 0;
		uint32_t physicalFlags = 0;
		Bytes virtualBlock;
		Bytes physicalBlock;
	};

	bool ReadHeader(const Bytes& file, uint32_t& version, uint32_t& virtualFlags, uint32_t& physicalFlags);
	bool Read(const Bytes& file, Resource& out, std::string& error);
	bool Write(const Resource& resource, Bytes& out, std::string& error);

	// Little-endian access into a byte buffer.
	template<class T>
	T Get(const Bytes& b, size_t offset)
	{
		T v{};
		if (offset + sizeof(T) <= b.size())
			memcpy(&v, b.data() + offset, sizeof(T));
		return v;
	}
	template<class T>
	void Put(Bytes& b, size_t offset, T v)
	{
		if (offset + sizeof(T) <= b.size())
			memcpy(b.data() + offset, &v, sizeof(T));
	}
}
