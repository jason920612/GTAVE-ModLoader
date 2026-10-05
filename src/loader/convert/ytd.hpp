#pragma once
#include <optional>
#include <string>

#include "resource.hpp"

namespace loader::convert
{
	constexpr uint32_t kLegacyTxdVersion = 13;
	constexpr uint32_t kEnhancedTxdVersion = 5;

	// Legacy texture dictionary (.ytd, version 13) -> Enhanced (version 5). Port of tools/convert_ytd.py;
	// formats in research/phase0.md §12-13 and §16.
	bool ConvertTextureDictionary(const Bytes& legacy, Bytes& out, std::string& error);

	// Shared with the fragment converter (textures embedded in models).
	struct TextureFormat
	{
		uint8_t dxgi;
		uint32_t unit; // bytes per 4x4 block, or per pixel
		bool block;
	};
	// D3D9 format (FourCC or enum) of legacy textures -> DXGI.
	std::optional<TextureFormat> TextureFormatFromD3D(uint32_t d3d);
	// Bytes of all mips, back to back.
	size_t MipChainSize(const TextureFormat& format, uint32_t width, uint32_t height, uint32_t mips);

	struct TextureHeader
	{
		uint64_t name;      // pointer to the name
		uint16_t width, height, depth;
		uint8_t mips;
		TextureFormat format;
		uint32_t stored;    // 4 KB aligned pixel bytes
		uint64_t data;      // pointer to the pixels
		bool renderTarget;  // "script_rt_*"
		bool normalMap;     // "*_n"
	};
	// Writes an Enhanced texture (0x80, view at +0x58) at `offset`; `self` is its own pointer.
	void WriteTextureHeader(Bytes& b, size_t offset, uint64_t self, const TextureHeader& t);
	bool IsRenderTargetName(std::string_view name);
	bool IsNormalMapName(std::string_view name);
}
