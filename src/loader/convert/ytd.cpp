#include "ytd.hpp"

#include <algorithm>
#include <format>
#include <optional>

namespace loader::convert
{
	std::optional<TextureFormat> TextureFormatFromD3D(uint32_t d3d)
	{
		switch (d3d)
		{
		case 0x31545844: return TextureFormat{71, 8, true};  // DXT1 -> BC1_UNORM
		case 0x33545844: return TextureFormat{74, 16, true}; // DXT3 -> BC2_UNORM
		case 0x35545844: return TextureFormat{77, 16, true}; // DXT5 -> BC3_UNORM
		case 0x31495441: return TextureFormat{80, 8, true};  // ATI1 -> BC4_UNORM
		case 0x32495441: return TextureFormat{83, 16, true}; // ATI2 -> BC5_UNORM
		case 0x20374342: return TextureFormat{98, 16, true}; // "BC7 " -> BC7_UNORM
		case 21: return TextureFormat{87, 4, false};          // A8R8G8B8 -> B8G8R8A8_UNORM
		case 22: return TextureFormat{88, 4, false};          // X8R8G8B8 -> B8G8R8X8_UNORM
		case 32: return TextureFormat{28, 4, false};          // A8B8G8R8 -> R8G8B8A8_UNORM
		case 25: return TextureFormat{86, 2, false};          // A1R5G5B5 -> B5G5R5A1_UNORM
		case 28: return TextureFormat{65, 1, false};          // A8 -> A8_UNORM
		case 50: return TextureFormat{61, 1, false};          // L8 -> R8_UNORM
		default: return std::nullopt;
		}
	}

	size_t MipChainSize(const TextureFormat& f, uint32_t width, uint32_t height, uint32_t mips)
	{
		size_t size = 0;
		for (uint32_t m = 0, w = width, h = height; m < mips; ++m, w = std::max(1u, w / 2), h = std::max(1u, h / 2))
			size += f.block ? static_cast<size_t>(std::max(1u, (w + 3) / 4)) * std::max(1u, (h + 3) / 4) * f.unit : static_cast<size_t>(w) * h * f.unit;
		return size;
	}

	namespace
	{
		std::string LowerText(std::string_view text)
		{
			std::string s(text);
			std::transform(s.begin(), s.end(), s.begin(), [](char c) { return static_cast<char>(c >= 'A' && c <= 'Z' ? c + 32 : c); });
			return s;
		}
	}

	bool IsRenderTargetName(std::string_view name)
	{
		return LowerText(name).starts_with("script_rt_");
	}

	namespace
	{
		void Colour565(uint16_t c, uint8_t out[4])
		{
			out[2] = static_cast<uint8_t>(((c >> 11) & 31) * 255 / 31); // B G R A order below
			out[1] = static_cast<uint8_t>(((c >> 5) & 63) * 255 / 63);
			out[0] = static_cast<uint8_t>((c & 31) * 255 / 31);
			out[3] = 255;
		}

		// One 4x4 colour block (BC1 layout) into BGRA texels.
		void DecodeColourBlock(const uint8_t* block, bool allowTransparent, uint8_t texels[16][4])
		{
			const uint16_t c0 = static_cast<uint16_t>(block[0] | block[1] << 8), c1 = static_cast<uint16_t>(block[2] | block[3] << 8);
			uint8_t palette[4][4];
			uint8_t a[4], b[4];
			Colour565(c0, a);
			Colour565(c1, b);
			for (int k = 0; k < 4; ++k)
			{
				palette[0][k] = a[k];
				palette[1][k] = b[k];
				if (c0 > c1 || !allowTransparent)
				{
					palette[2][k] = static_cast<uint8_t>((2 * a[k] + b[k]) / 3);
					palette[3][k] = static_cast<uint8_t>((a[k] + 2 * b[k]) / 3);
				}
				else
				{
					palette[2][k] = static_cast<uint8_t>((a[k] + b[k]) / 2);
					palette[3][k] = 0;
				}
			}
			if (!(c0 > c1 || !allowTransparent))
				palette[3][3] = 0;
			const uint32_t bits = static_cast<uint32_t>(block[4] | block[5] << 8 | block[6] << 16 | block[7] << 24);
			for (int i = 0; i < 16; ++i)
				memcpy(texels[i], palette[(bits >> (2 * i)) & 3], 4);
		}
	}

	void MakeRenderTargetFormat(TextureFormat& format, uint8_t& mips, uint16_t width, uint16_t height, Bytes& pixels)
	{
		if (!format.block)
			return;
		Bytes out(static_cast<size_t>(width) * height * 4, 0);
		const uint32_t blocksX = std::max(1u, (width + 3u) / 4), blocksY = std::max(1u, (height + 3u) / 4);
		for (uint32_t by = 0; by < blocksY; ++by)
			for (uint32_t bx = 0; bx < blocksX; ++bx)
			{
				const size_t at = (static_cast<size_t>(by) * blocksX + bx) * format.unit;
				if (at + format.unit > pixels.size())
					continue;
				const uint8_t* block = pixels.data() + at;
				uint8_t texels[16][4] = {};
				if (format.dxgi == 71) // BC1
					DecodeColourBlock(block, true, texels);
				else if (format.dxgi == 74) // BC2: explicit 4-bit alpha, then colour
				{
					DecodeColourBlock(block + 8, false, texels);
					for (int i = 0; i < 16; ++i)
						texels[i][3] = static_cast<uint8_t>(((block[i / 2] >> (4 * (i & 1))) & 15) * 17);
				}
				else if (format.dxgi == 77) // BC3: interpolated alpha, then colour
				{
					DecodeColourBlock(block + 8, false, texels);
					const uint8_t a0 = block[0], a1 = block[1];
					uint8_t alpha[8] = {a0, a1};
					for (int k = 2; k < 8; ++k)
						alpha[k] = a0 > a1 ? static_cast<uint8_t>(((8 - k) * a0 + (k - 1) * a1) / 7)
						                   : k < 6 ? static_cast<uint8_t>(((6 - k) * a0 + (k - 1) * a1) / 5) : (k == 6 ? 0 : 255);
					uint64_t bits = 0;
					for (int k = 0; k < 6; ++k)
						bits |= static_cast<uint64_t>(block[2 + k]) << (8 * k);
					for (int i = 0; i < 16; ++i)
						texels[i][3] = alpha[(bits >> (3 * i)) & 7];
				}
				else
					continue; // other compressed formats: left blank
				for (int i = 0; i < 16; ++i)
				{
					const uint32_t x = bx * 4 + (i & 3), y = by * 4 + (i >> 2);
					if (x < width && y < height)
						memcpy(out.data() + (static_cast<size_t>(y) * width + x) * 4, texels[i], 4);
				}
			}
		pixels = std::move(out);
		format = TextureFormat{87, 4, false}; // B8G8R8A8_UNORM, as the game's own dial textures
		mips = 1;
	}

	bool IsNormalMapName(std::string_view name)
	{
		return LowerText(name).ends_with("_n");
	}

	void WriteTextureHeader(Bytes& b, size_t o, uint64_t self, const TextureHeader& t)
	{
		std::fill(b.begin() + o, b.begin() + o + 0x80, uint8_t{0});
		Put<uint64_t>(b, o, 0x1406B7940); // file-form type id of the game's own textures
		Put<uint32_t>(b, o + 0x08, t.stored / t.format.unit);
		Put<uint16_t>(b, o + 0x0C, static_cast<uint16_t>(t.format.unit));
		// "script_rt_*" textures become render targets (vehicle dials drawn by scripts): the game's own files
		// mark them render-target capable; a render target view of a plain texture removes the device.
		Put<uint32_t>(b, o + 0x10, t.renderTarget ? 0x01A60228 : 0x01A70208);
		Put<uint16_t>(b, o + 0x18, t.width);
		Put<uint16_t>(b, o + 0x1A, t.height);
		Put<uint16_t>(b, o + 0x1C, t.depth);
		b[o + 0x1E] = 1;
		b[o + 0x1F] = t.format.dxgi;
		b[o + 0x20] = 0xFF;
		b[o + 0x22] = t.mips;
		b[o + 0x26] = 1;
		Put<uint64_t>(b, o + 0x28, t.name);
		Put<uint64_t>(b, o + 0x30, self + 0x58);
		Put<uint64_t>(b, o + 0x38, t.data);
		Put<uint16_t>(b, o + 0x40, t.renderTarget ? 0x21A : (t.normalMap ? 0x216 : 0x214));
		Put<uint16_t>(b, o + 0x42, t.renderTarget ? 0x40 : 0x80);
		Put<uint16_t>(b, o + 0x44, 2);
	}

	namespace
	{
		struct Texture
		{
			std::string name;
			uint16_t width, height, depth;
			uint8_t mips;
			TextureFormat format;
			Bytes pixels;
			uint32_t stored = 0; // 4 KB aligned
			uint32_t offset = 0; // in the physical block
			uint32_t nameOffset = 0;
		};

		std::string CString(const Bytes& b, size_t offset)
		{
			std::string s;
			for (; offset < b.size() && b[offset]; ++offset)
				s += static_cast<char>(b[offset]);
			return s;
		}

	}

	bool ConvertTextureDictionary(const Bytes& legacy, Bytes& out, std::string& error)
	{
		Resource in;
		if (!Read(legacy, in, error))
			return false;
		if (in.version != kLegacyTxdVersion)
		{
			error = std::format("not a legacy texture dictionary (version {})", in.version);
			return false;
		}
		const Bytes& v = in.virtualBlock;
		const auto ptr = [](uint64_t p) { return static_cast<size_t>(p - kVirtual); };

		const uint16_t count = Get<uint16_t>(v, 0x38);
		const size_t list = ptr(Get<uint64_t>(v, 0x30));
		std::vector<Texture> textures;
		for (uint16_t i = 0; i < count; ++i)
		{
			const size_t o = ptr(Get<uint64_t>(v, list + 8 * i));
			if (o + 0x78 > v.size())
			{
				error = "corrupt texture list";
				return false;
			}
			Texture t;
			t.name = CString(v, ptr(Get<uint64_t>(v, o + 0x28)));
			t.width = Get<uint16_t>(v, o + 0x50);
			t.height = Get<uint16_t>(v, o + 0x52);
			t.depth = std::max<uint16_t>(1, Get<uint16_t>(v, o + 0x54));
			t.mips = std::max<uint8_t>(1, v[o + 0x5D]);
			const auto format = TextureFormatFromD3D(Get<uint32_t>(v, o + 0x58));
			if (!format)
			{
				error = std::format("{}: unsupported format {:#x}", t.name, Get<uint32_t>(v, o + 0x58));
				return false;
			}
			t.format = *format;
			const size_t size = MipChainSize(t.format, t.width, t.height, t.mips);
			const uint64_t data = Get<uint64_t>(v, o + 0x70);
			const size_t start = static_cast<size_t>(data - kPhysical);
			if (data < kPhysical || start + size > in.physicalBlock.size())
			{
				error = std::format("{}: pixel data out of range", t.name);
				return false;
			}
			t.pixels.assign(in.physicalBlock.begin() + start, in.physicalBlock.begin() + start + size);
			if (IsRenderTargetName(t.name))
				MakeRenderTargetFormat(t.format, t.mips, t.width, t.height, t.pixels);
			t.stored = static_cast<uint32_t>((t.pixels.size() + 0xFFF) & ~size_t{0xFFF});
			textures.push_back(std::move(t));
		}
		std::sort(textures.begin(), textures.end(), [](const Texture& a, const Texture& b) { return Joaat(a.name) < Joaat(b.name); });

		// Physical block: the game loads each page into its own allocation, so a texture must not cross a
		// page boundary. Equal pages large enough for the biggest texture, filled first-fit, largest first.
		uint32_t largest = 0x1000;
		for (const Texture& t : textures)
			largest = std::max(largest, t.stored);
		uint32_t shift = 0;
		while ((0x2000u << shift) < largest)
			++shift;
		std::vector<Texture*> order;
		for (Texture& t : textures)
			order.push_back(&t);
		std::stable_sort(order.begin(), order.end(), [](const Texture* a, const Texture* b) { return a->stored > b->stored; });
		std::vector<uint32_t> freeBytes;
		for (;; ++shift)
		{
			const uint32_t page = 0x2000u << shift;
			freeBytes.clear();
			for (Texture* t : order)
			{
				bool placed = false;
				for (size_t i = 0; i < freeBytes.size() && !placed; ++i)
					if (freeBytes[i] >= t->stored)
					{
						t->offset = static_cast<uint32_t>(i) * page + (page - freeBytes[i]);
						freeBytes[i] -= t->stored;
						placed = true;
					}
				if (!placed)
				{
					t->offset = static_cast<uint32_t>(freeBytes.size()) * page;
					freeBytes.push_back(page - t->stored);
				}
			}
			if (freeBytes.size() <= 127)
				break;
		}
		Resource res;
		res.version = kEnhancedTxdVersion;
		res.physicalFlags = shift | (static_cast<uint32_t>(freeBytes.size()) << 17);
		res.physicalBlock.assign(BlockSize(res.physicalFlags), 0);
		for (const Texture& t : textures)
			std::copy(t.pixels.begin(), t.pixels.end(), res.physicalBlock.begin() + t.offset);

		// Virtual block: header, pointer list, hash list, textures (0x80 each), page map, names. One page.
		const size_t n = textures.size();
		const size_t offPointers = 0x40;
		const size_t offHashes = (offPointers + 8 * n + 15) & ~size_t{15};
		const size_t offTextures = (offHashes + 4 * n + 15) & ~size_t{15};
		const size_t offPageMap = offTextures + 0x80 * n;
		const size_t offNames = (offPageMap + 16 + 8 * (1 + PageCount(res.physicalFlags)) + 15) & ~size_t{15};
		size_t end = offNames;
		for (Texture& t : textures)
		{
			t.nameOffset = static_cast<uint32_t>(end);
			end += t.name.size() + 1;
		}
		uint32_t vshift = 0;
		while ((0x2000u << vshift) < end)
			++vshift;
		if (vshift > 15)
		{
			error = "too many textures";
			return false;
		}
		res.virtualFlags = vshift | (1u << 17);
		Bytes& b = res.virtualBlock;
		b.assign(BlockSize(res.virtualFlags), 0);
		Put<uint64_t>(b, 0x08, kVirtual + offPageMap);
		Put<uint64_t>(b, 0x18, 1);
		Put<uint64_t>(b, 0x20, kVirtual + offHashes);
		Put<uint16_t>(b, 0x28, static_cast<uint16_t>(n));
		Put<uint16_t>(b, 0x2A, static_cast<uint16_t>(n));
		Put<uint64_t>(b, 0x30, kVirtual + offPointers);
		Put<uint16_t>(b, 0x38, static_cast<uint16_t>(n));
		Put<uint16_t>(b, 0x3A, static_cast<uint16_t>(n));
		for (size_t i = 0; i < n; ++i)
		{
			const Texture& t = textures[i];
			const size_t o = offTextures + 0x80 * i;
			Put<uint64_t>(b, offPointers + 8 * i, kVirtual + o);
			Put<uint32_t>(b, offHashes + 4 * i, Joaat(t.name));
			WriteTextureHeader(b, o, kVirtual + o, {kVirtual + t.nameOffset, t.width, t.height, t.depth, t.mips, t.format, t.stored,
			    kPhysical + t.offset, IsRenderTargetName(t.name), IsNormalMapName(t.name)});
			std::copy(t.name.begin(), t.name.end(), b.begin() + t.nameOffset);
		}
		b[offPageMap + 8] = static_cast<uint8_t>(PageCount(res.virtualFlags));
		b[offPageMap + 9] = static_cast<uint8_t>(PageCount(res.physicalFlags));
		res.virtualFlags |= (kEnhancedTxdVersion >> 4) << 28;
		res.physicalFlags |= (kEnhancedTxdVersion & 0xF) << 28;
		return Write(res, out, error);
	}
}
