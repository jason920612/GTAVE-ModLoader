#include "texture.hpp"

#include <algorithm>
#include <cmath>

#include "raster.hpp"

namespace worldgen::texture
{
	std::array<float, 4> Image::Sample(float u, float v) const
	{
		const float fu = u - 0.5f, fv = v - 0.5f;
		const int x0 = static_cast<int>(std::floor(fu)), y0 = static_cast<int>(std::floor(fv));
		const float tx = fu - x0, ty = fv - y0;
		std::array<float, 4> out{};
		for (int k = 0; k < 4; ++k)
		{
			const int x = ((x0 + (k & 1)) % width + width) % width, y = ((y0 + (k >> 1)) % height + height) % height;
			const float w = ((k & 1) ? tx : 1 - tx) * ((k >> 1) ? ty : 1 - ty);
			const auto& p = At(x, y);
			for (int c = 0; c < 4; ++c)
				out[c] += w * p[c];
		}
		return out;
	}

	namespace
	{
		uint16_t To565(const float c[3])
		{
			const int r = std::clamp(static_cast<int>(std::lround(c[0] * 31 / 255)), 0, 31);
			const int g = std::clamp(static_cast<int>(std::lround(c[1] * 63 / 255)), 0, 63);
			const int b = std::clamp(static_cast<int>(std::lround(c[2] * 31 / 255)), 0, 31);
			return static_cast<uint16_t>(r << 11 | g << 5 | b);
		}

		void From565(uint16_t v, float out[3])
		{
			out[0] = static_cast<float>((v >> 11) & 31) * 255 / 31;
			out[1] = static_cast<float>((v >> 5) & 63) * 255 / 63;
			out[2] = static_cast<float>(v & 31) * 255 / 31;
		}

		// one 4 x 4 block of colours (BC1, four-colour mode)
		void ColourBlock(const std::array<uint8_t, 4> (&px)[16], uint8_t* out)
		{
			float mean[3] = {};
			for (const auto& p : px)
				for (int c = 0; c < 3; ++c)
					mean[c] += p[c] / 16.0f;
			float cov[6] = {}; // xx xy xz yy yz zz
			for (const auto& p : px)
			{
				const float d[3] = {p[0] - mean[0], p[1] - mean[1], p[2] - mean[2]};
				cov[0] += d[0] * d[0], cov[1] += d[0] * d[1], cov[2] += d[0] * d[2];
				cov[3] += d[1] * d[1], cov[4] += d[1] * d[2], cov[5] += d[2] * d[2];
			}
			float axis[3] = {0.57f, 0.58f, 0.57f};
			for (int it = 0; it < 6; ++it)
			{
				const float n[3] = {cov[0] * axis[0] + cov[1] * axis[1] + cov[2] * axis[2], cov[1] * axis[0] + cov[3] * axis[1] + cov[4] * axis[2],
				    cov[2] * axis[0] + cov[4] * axis[1] + cov[5] * axis[2]};
				const float l = std::sqrt(n[0] * n[0] + n[1] * n[1] + n[2] * n[2]);
				if (l < 1e-6f)
					break;
				for (int c = 0; c < 3; ++c)
					axis[c] = n[c] / l;
			}
			float lo = 1e30f, hi = -1e30f;
			for (const auto& p : px)
			{
				const float t = (p[0] - mean[0]) * axis[0] + (p[1] - mean[1]) * axis[1] + (p[2] - mean[2]) * axis[2];
				lo = std::min(lo, t), hi = std::max(hi, t);
			}
			// (a little inset: the ends are rarely hit exactly)
			const float inset = (hi - lo) / 32;
			lo += inset, hi -= inset;
			float e0[3], e1[3];
			for (int c = 0; c < 3; ++c)
				e0[c] = mean[c] + axis[c] * hi, e1[c] = mean[c] + axis[c] * lo;
			uint16_t c0 = To565(e0), c1 = To565(e1);
			if (c0 < c1)
				std::swap(c0, c1);
			out[0] = static_cast<uint8_t>(c0), out[1] = static_cast<uint8_t>(c0 >> 8), out[2] = static_cast<uint8_t>(c1), out[3] = static_cast<uint8_t>(c1 >> 8);
			uint32_t bits = 0;
			if (c0 != c1)
			{
				float pal[4][3];
				From565(c0, pal[0]);
				From565(c1, pal[1]);
				for (int c = 0; c < 3; ++c)
					pal[2][c] = (2 * pal[0][c] + pal[1][c]) / 3, pal[3][c] = (pal[0][c] + 2 * pal[1][c]) / 3;
				for (int i = 0; i < 16; ++i)
				{
					int best = 0;
					float bestD = 1e30f;
					for (int k = 0; k < 4; ++k)
					{
						const float dr = px[i][0] - pal[k][0], dg = px[i][1] - pal[k][1], db = px[i][2] - pal[k][2];
						const float d = dr * dr * 0.3f + dg * dg * 0.59f + db * db * 0.11f;
						if (d < bestD)
							bestD = d, best = k;
					}
					bits |= static_cast<uint32_t>(best) << (2 * i);
				}
			}
			for (int k = 0; k < 4; ++k)
				out[4 + k] = static_cast<uint8_t>(bits >> (8 * k));
		}

		void AlphaBlock(const std::array<uint8_t, 4> (&px)[16], uint8_t* out)
		{
			uint8_t lo = 255, hi = 0;
			for (const auto& p : px)
				lo = std::min(lo, p[3]), hi = std::max(hi, p[3]);
			out[0] = hi, out[1] = lo;
			uint8_t pal[8] = {hi, lo};
			for (int k = 2; k < 8; ++k)
				pal[k] = static_cast<uint8_t>(((8 - k) * hi + (k - 1) * lo) / 7);
			uint64_t bits = 0;
			for (int i = 0; i < 16; ++i)
			{
				int best = 0, bestD = 1 << 30;
				for (int k = 0; k < 8; ++k)
					if (const int d = std::abs(px[i][3] - pal[k]); d < bestD)
						bestD = d, best = k;
				bits |= static_cast<uint64_t>(best) << (3 * i);
			}
			for (int k = 0; k < 6; ++k)
				out[2 + k] = static_cast<uint8_t>(bits >> (8 * k));
		}

		Image Half(const Image& in)
		{
			Image out(std::max(1, in.width / 2), std::max(1, in.height / 2));
			for (int y = 0; y < out.height; ++y)
				for (int x = 0; x < out.width; ++x)
					for (int c = 0; c < 4; ++c)
					{
						int s = 0, n = 0;
						for (int k = 0; k < 4; ++k)
						{
							const int sx = std::min(in.width - 1, 2 * x + (k & 1)), sy = std::min(in.height - 1, 2 * y + (k >> 1));
							s += in.At(sx, sy)[c], ++n;
						}
						out.At(x, y)[c] = static_cast<uint8_t>((s + n / 2) / n);
					}
			return out;
		}
	}

	namespace
	{
		// A cut-out's clear texels (alpha under 8) the colour of the nearest ones not clear, outwards a texel at a time: the
		// game's bilinear filtering had pulled their dark colour into the leaves' edges ("leaf cards with dark speckled
		// edges").
		void Bleed(Image& im)
		{
			std::vector<uint8_t> done(im.pixels.size());
			std::vector<size_t> front;
			for (size_t i = 0; i < im.pixels.size(); ++i)
				if (im.pixels[i][3] >= 8)
					done[i] = 1, front.push_back(i);
			while (!front.empty())
			{
				std::vector<size_t> next;
				for (const size_t i : front)
				{
					const int x = static_cast<int>(i % im.width), y = static_cast<int>(i / im.width);
					for (int k = 0; k < 4; ++k)
					{
						const int nx = x + (k == 0 ? 1 : k == 1 ? -1 : 0), ny = y + (k == 2 ? 1 : k == 3 ? -1 : 0);
						if (nx < 0 || ny < 0 || nx >= im.width || ny >= im.height)
							continue;
						const size_t j = static_cast<size_t>(ny) * im.width + nx;
						if (done[j])
							continue;
						done[j] = 1;
						for (int c = 0; c < 3; ++c)
							im.pixels[j][c] = im.pixels[i][c];
						next.push_back(j);
					}
				}
				front = std::move(next);
			}
		}

		// A cut-out's next level: colour by alpha, alpha scaled to keep `coverage` (the share over half)
		Image HalfCutout(const Image& in, float coverage)
		{
			Image out(std::max(1, in.width / 2), std::max(1, in.height / 2));
			for (int y = 0; y < out.height; ++y)
				for (int x = 0; x < out.width; ++x)
				{
					int sa = 0, sc[3]{}, sp[3]{};
					for (int k = 0; k < 4; ++k)
					{
						const auto& p = in.At(std::min(in.width - 1, 2 * x + (k & 1)), std::min(in.height - 1, 2 * y + (k >> 1)));
						sa += p[3];
						for (int c = 0; c < 3; ++c)
							sc[c] += p[c] * p[3], sp[c] += p[c];
					}
					auto& q = out.At(x, y);
					for (int c = 0; c < 3; ++c)
						q[c] = static_cast<uint8_t>(sa > 0 ? sc[c] / sa : sp[c] / 4);
					q[3] = static_cast<uint8_t>(sa / 4);
				}
			// (the alpha's scale that keeps the coverage: halved until it is found)
			float lo = 0.5f, hi = 8.0f;
			for (int i = 0; i < 20; ++i)
			{
				const float k = (lo + hi) / 2;
				size_t over = 0;
				for (const auto& p : out.pixels)
					over += p[3] * k >= 128;
				(static_cast<float>(over) / out.pixels.size() < coverage ? lo : hi) = k;
			}
			for (auto& p : out.pixels)
				p[3] = static_cast<uint8_t>(std::min(255.0f, p[3] * hi));
			return out;
		}
	}

	loader::convert::DictionaryTexture Make(const std::string& name, const Image& image, Format format, bool cutout)
	{
		float coverage = 0;
		if (cutout)
		{
			size_t over = 0;
			for (const auto& p : image.pixels)
				over += p[3] >= 128;
			coverage = static_cast<float>(over) / std::max<size_t>(1, image.pixels.size());
		}
		loader::convert::DictionaryTexture t;
		t.name = name;
		t.map = true;
		t.width = static_cast<uint16_t>(image.width);
		t.height = static_cast<uint16_t>(image.height);
		t.format = format == Format::Bc1 ? loader::convert::TextureFormat{71, 8, true}
		           : format == Format::Bc3 ? loader::convert::TextureFormat{77, 16, true}
		                                   : loader::convert::TextureFormat{87, 4, false};
		Image level = image;
		uint8_t mips = 0;
		while (true)
		{
			++mips;
			if (cutout)
				Bleed(level);
			if (format == Format::Bgra)
				for (const auto& p : level.pixels)
					t.pixels.insert(t.pixels.end(), {p[2], p[1], p[0], p[3]});
			else
				for (int by = 0; by < std::max(1, (level.height + 3) / 4); ++by)
					for (int bx = 0; bx < std::max(1, (level.width + 3) / 4); ++bx)
					{
						std::array<uint8_t, 4> px[16];
						for (int i = 0; i < 16; ++i)
							px[i] = level.At(std::min(level.width - 1, bx * 4 + (i & 3)), std::min(level.height - 1, by * 4 + (i >> 2)));
						uint8_t block[16];
						if (format == Format::Bc3)
						{
							AlphaBlock(px, block);
							ColourBlock(px, block + 8);
							t.pixels.insert(t.pixels.end(), block, block + 16);
						}
						else
						{
							ColourBlock(px, block);
							t.pixels.insert(t.pixels.end(), block, block + 8);
						}
					}
			// (block formats stop at 4 x 4)
			if ((level.width <= 4 || level.height <= 4) && format != Format::Bgra)
				break;
			if (level.width == 1 && level.height == 1)
				break;
			// (a cut-out's smaller levels keep a sixth more of it over half each, to 0.92: kept as it was, a far crown's cards
			// had thinned to "grainy dithered leaf speckle, see-through clumps" - the reviewers want "soft continuous masses")
			if (cutout)
				coverage = std::min(0.92f, coverage * (1 + 1.0f / 6));
			level = cutout ? HalfCutout(level, coverage) : Half(level);
		}
		t.mips = mips;
		return t;
	}

	bool WritePng(const std::filesystem::path& file, const Image& image, std::string& error)
	{
		std::vector<std::array<uint8_t, 3>> rgb(image.pixels.size());
		// (raster::WritePng writes rows bottom to top)
		for (int y = 0; y < image.height; ++y)
			for (int x = 0; x < image.width; ++x)
			{
				const auto& p = image.At(x, y);
				rgb[static_cast<size_t>(image.height - 1 - y) * image.width + x] = {p[0], p[1], p[2]};
			}
		return raster::WritePng(file, image.width, image.height, rgb, error);
	}
}
