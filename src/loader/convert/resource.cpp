#include "resource.hpp"

#include <Windows.h>

#include <cstring>

namespace loader::convert
{
	namespace
	{
		constexpr uint32_t kMagic = 0x37435352; // 'RSC7'

		struct ZStream
		{
			const uint8_t* next_in;
			uint32_t avail_in;
			uint32_t total_in;
			uint8_t* next_out;
			uint32_t avail_out;
			uint32_t total_out;
			const char* msg;
			void* state;
			void* zalloc;
			void* zfree;
			void* opaque;
			int data_type;
			uint32_t adler;
			uint32_t reserved;
		};

		struct Zlib
		{
			using InflateInitFn = int(__cdecl*)(ZStream*, int, const char*, int);
			using DeflateInitFn = int(__cdecl*)(ZStream*, int, int, int, int, int, const char*, int);
			using StepFn = int(__cdecl*)(ZStream*, int);
			using EndFn = int(__cdecl*)(ZStream*);
			InflateInitFn inflateInit = nullptr;
			StepFn inflate = nullptr;
			EndFn inflateEnd = nullptr;
			DeflateInitFn deflateInit = nullptr;
			StepFn deflate = nullptr;
			EndFn deflateEnd = nullptr;

			Zlib()
			{
				const HMODULE dll = LoadLibraryW(L"zlib1.dll");
				if (!dll)
					return;
				inflateInit = reinterpret_cast<InflateInitFn>(GetProcAddress(dll, "inflateInit2_"));
				inflate = reinterpret_cast<StepFn>(GetProcAddress(dll, "inflate"));
				inflateEnd = reinterpret_cast<EndFn>(GetProcAddress(dll, "inflateEnd"));
				deflateInit = reinterpret_cast<DeflateInitFn>(GetProcAddress(dll, "deflateInit2_"));
				deflate = reinterpret_cast<StepFn>(GetProcAddress(dll, "deflate"));
				deflateEnd = reinterpret_cast<EndFn>(GetProcAddress(dll, "deflateEnd"));
			}

			bool Ok() const { return inflateInit && inflate && inflateEnd && deflateInit && deflate && deflateEnd; }
		};

		const Zlib& Z()
		{
			static const Zlib zlib;
			return zlib;
		}
	}

	uint32_t BlockSize(uint32_t flags)
	{
		const uint32_t base = 0x2000u << (flags & 0xF);
		const uint32_t count = (flags & 0x10) + ((flags >> 2) & 0x18) + ((flags >> 5) & 0x3C) + ((flags >> 10) & 0x7E) + ((flags >> 17) & 0x7F);
		uint32_t size = count * base;
		if (flags & 0x8000000)
			size += base / 16;
		if (flags & 0x4000000)
			size += base / 8;
		if (flags & 0x2000000)
			size += base / 4;
		if (flags & 0x1000000)
			size += base / 2;
		return size;
	}

	std::vector<uint32_t> PageList(uint32_t flags)
	{
		const uint32_t base = 0x2000u << (flags & 0xF);
		std::vector<uint32_t> pages;
		pages.insert(pages.end(), (flags >> 4) & 1, base * 16);
		pages.insert(pages.end(), (flags >> 5) & 3, base * 8);
		pages.insert(pages.end(), (flags >> 7) & 0xF, base * 4);
		pages.insert(pages.end(), (flags >> 11) & 0x3F, base * 2);
		pages.insert(pages.end(), (flags >> 17) & 0x7F, base);
		for (const auto [bit, div] : {std::pair{0x1000000u, 2u}, {0x2000000u, 4u}, {0x4000000u, 8u}, {0x8000000u, 16u}})
			if (flags & bit)
				pages.push_back(base / div);
		return pages;
	}

	uint32_t PageCount(uint32_t flags)
	{
		return static_cast<uint32_t>(PageList(flags).size());
	}

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
		return h + (h << 15);
	}

	bool Inflate(const uint8_t* data, size_t size, Bytes& out)
	{
		const Zlib& z = Z();
		if (!z.Ok())
			return false;
		ZStream s{};
		s.next_in = data;
		s.avail_in = static_cast<uint32_t>(size);
		s.next_out = out.data();
		s.avail_out = static_cast<uint32_t>(out.size());
		if (z.inflateInit(&s, -15, "1.2.11", static_cast<int>(sizeof(ZStream))) != 0)
			return false;
		const int status = z.inflate(&s, 4 /* Z_FINISH */);
		z.inflateEnd(&s);
		return (status == 1 /* Z_STREAM_END */ || status == 0) && s.total_out == out.size();
	}

	size_t InflatedSize(const uint8_t* data, size_t size, size_t limit)
	{
		const Zlib& z = Z();
		if (!z.Ok())
			return 0;
		Bytes out(limit);
		ZStream s{};
		s.next_in = data;
		s.avail_in = static_cast<uint32_t>(size);
		s.next_out = out.data();
		s.avail_out = static_cast<uint32_t>(out.size());
		if (z.inflateInit(&s, -15, "1.2.11", static_cast<int>(sizeof(ZStream))) != 0)
			return 0;
		const int status = z.inflate(&s, 0);
		z.inflateEnd(&s);
		return status >= 0 ? s.total_out : 0;
	}

	bool Deflate(const Bytes& data, Bytes& out)
	{
		const Zlib& z = Z();
		if (!z.Ok())
			return false;
		ZStream s{};
		if (z.deflateInit(&s, 9, 8 /* Z_DEFLATED */, -15, 8, 0, "1.2.11", static_cast<int>(sizeof(ZStream))) != 0)
			return false;
		out.resize(data.size() + data.size() / 1000 + 64);
		s.next_in = data.data();
		s.avail_in = static_cast<uint32_t>(data.size());
		s.next_out = out.data();
		s.avail_out = static_cast<uint32_t>(out.size());
		const int status = z.deflate(&s, 4 /* Z_FINISH */);
		z.deflateEnd(&s);
		if (status != 1 /* Z_STREAM_END */)
			return false;
		out.resize(s.total_out);
		return true;
	}

	bool ReadHeader(const Bytes& file, uint32_t& version, uint32_t& virtualFlags, uint32_t& physicalFlags)
	{
		if (file.size() < 16 || Get<uint32_t>(file, 0) != kMagic)
			return false;
		version = Get<uint32_t>(file, 4);
		virtualFlags = Get<uint32_t>(file, 8);
		physicalFlags = Get<uint32_t>(file, 12);
		return true;
	}

	bool Read(const Bytes& file, Resource& out, std::string& error)
	{
		if (!ReadHeader(file, out.version, out.virtualFlags, out.physicalFlags))
		{
			error = "not a resource";
			return false;
		}
		const uint32_t virtualSize = BlockSize(out.virtualFlags), physicalSize = BlockSize(out.physicalFlags);
		Bytes plain(static_cast<size_t>(virtualSize) + physicalSize);
		if (!Inflate(file.data() + 16, file.size() - 16, plain))
		{
			error = "cannot decompress";
			return false;
		}
		out.virtualBlock.assign(plain.begin(), plain.begin() + virtualSize);
		out.physicalBlock.assign(plain.begin() + virtualSize, plain.end());
		return true;
	}

	bool Write(const Resource& resource, Bytes& out, std::string& error)
	{
		Bytes plain = resource.virtualBlock;
		plain.insert(plain.end(), resource.physicalBlock.begin(), resource.physicalBlock.end());
		Bytes packed;
		if (!Deflate(plain, packed))
		{
			error = "cannot compress";
			return false;
		}
		out.resize(16);
		Put<uint32_t>(out, 0, kMagic);
		Put<uint32_t>(out, 4, resource.version);
		Put<uint32_t>(out, 8, resource.virtualFlags);
		Put<uint32_t>(out, 12, resource.physicalFlags);
		out.insert(out.end(), packed.begin(), packed.end());
		return true;
	}
}
