#pragma once
#include <array>
#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

#include "convert/ytd.hpp"

// Pictures of our own for texture dictionaries (research/phase0.md §61.18): RGBA images, their mip chains, block
// compression (BC1 a range fit along each block's principal axis; BC3 with its alpha block) and PNG pictures for
// checking.
namespace worldgen::texture
{
	struct Image
	{
		int width = 0, height = 0;
		std::vector<std::array<uint8_t, 4>> pixels; // rows top to bottom, red green blue alpha

		Image() = default;
		Image(int w, int h, std::array<uint8_t, 4> fill = {0, 0, 0, 255}) : width(w), height(h), pixels(static_cast<size_t>(w) * h, fill) {}
		std::array<uint8_t, 4>& At(int x, int y) { return pixels[static_cast<size_t>(y) * width + x]; }
		const std::array<uint8_t, 4>& At(int x, int y) const { return pixels[static_cast<size_t>(y) * width + x]; }
		// wrapping bilinear read (u, v in pixels)
		std::array<float, 4> Sample(float u, float v) const;
	};

	enum class Format
	{
		Bc1,  // colour (DXGI 71)
		Bc3,  // colour and alpha (77)
		Bgra, // uncompressed (87)
	};

	// A dictionary texture of the image with its full mip chain (sides of powers of two).
	loader::convert::DictionaryTexture Make(const std::string& name, const Image& image, Format format);
	bool WritePng(const std::filesystem::path& file, const Image& image, std::string& error);
}
