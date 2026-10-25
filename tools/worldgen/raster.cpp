#include "raster.hpp"

#include <fstream>
#include <limits>

#include "convert/resource.hpp"

namespace worldgen::raster
{
	namespace
	{
		struct Segment
		{
			P2 a, b;
		};

		// Segments in square buckets.
		struct Buckets
		{
			float x0 = 0, y0 = 0, size = 128;
			int nx = 0, ny = 0;
			std::vector<std::vector<uint32_t>> cells;
			std::vector<Segment> segments;

			void Build(const std::vector<std::vector<P2>>& lines, float minX, float minY, float maxX, float maxY)
			{
				x0 = minX, y0 = minY;
				nx = static_cast<int>((maxX - minX) / size) + 1, ny = static_cast<int>((maxY - minY) / size) + 1;
				cells.assign(static_cast<size_t>(nx) * ny, {});
				for (const auto& l : lines)
					for (size_t k = 0; k + 1 < l.size(); ++k)
					{
						const Segment s{l[k], l[k + 1]};
						// (only where it can matter: clipped to the buckets' area, a margin around it)
						const int i0 = std::max(0, static_cast<int>((std::min(s.a[0], s.b[0]) - x0) / size)),
						          i1 = std::min(nx - 1, static_cast<int>((std::max(s.a[0], s.b[0]) - x0) / size));
						const int j0 = std::max(0, static_cast<int>((std::min(s.a[1], s.b[1]) - y0) / size)),
						          j1 = std::min(ny - 1, static_cast<int>((std::max(s.a[1], s.b[1]) - y0) / size));
						if (i0 > i1 || j0 > j1)
							continue;
						const uint32_t id = static_cast<uint32_t>(segments.size());
						segments.push_back(s);
						for (int j = j0; j <= j1; ++j)
							for (int i = i0; i <= i1; ++i)
								cells[static_cast<size_t>(j) * nx + i].push_back(id);
					}
			}

			// The side of the nearest segment to p (1 left = land, 0 right) and its distance, searching outward ring by
			// ring up to maxRings; -1 when there is none.
			int Side(const P2& p, int maxRings, float& distance) const
			{
				const int ci = static_cast<int>((p[0] - x0) / size), cj = static_cast<int>((p[1] - y0) / size);
				float best = std::numeric_limits<float>::max();
				double weight = 0;
				bool found = false;
				for (int ring = 0; ring <= maxRings; ++ring)
				{
					if (found && (ring - 1) * size > best)
						break;
					for (int j = cj - ring; j <= cj + ring; ++j)
						for (int i = ci - ring; i <= ci + ring; ++i)
						{
							if (std::max(std::abs(i - ci), std::abs(j - cj)) != ring || i < 0 || j < 0 || i >= nx || j >= ny)
								continue;
							for (const uint32_t id : cells[static_cast<size_t>(j) * nx + i])
							{
								const Segment& s = segments[id];
								const float dx = s.b[0] - s.a[0], dy = s.b[1] - s.a[1], len2 = dx * dx + dy * dy;
								if (len2 <= 0)
									continue;
								const float t = std::clamp(((p[0] - s.a[0]) * dx + (p[1] - s.a[1]) * dy) / len2, 0.0f, 1.0f);
								const float ex = p[0] - (s.a[0] + t * dx), ey = p[1] - (s.a[1] + t * dy);
								const float d = std::sqrt(ex * ex + ey * ey);
								const double cross = (static_cast<double>(dx) * (p[1] - s.a[1]) - static_cast<double>(dy) * (p[0] - s.a[0])) /
								                     (std::sqrt(len2) * std::max(d, 1e-3f));
								if (d < best - 1e-3f)
									best = d, weight = cross, found = true;
								else if (d <= best + 1e-3f)
									weight += cross; // (a shared corner: the segments meeting there together)
							}
						}
				}
				distance = best;
				return found ? (weight > 0 ? 1 : 0) : -1;
			}
		};

		uint32_t Crc(const uint8_t* p, size_t n, uint32_t c = 0xFFFFFFFF)
		{
			static uint32_t table[256];
			static bool made = false;
			if (!made)
			{
				for (uint32_t i = 0; i < 256; ++i)
				{
					uint32_t v = i;
					for (int k = 0; k < 8; ++k)
						v = v & 1 ? 0xEDB88320 ^ (v >> 1) : v >> 1;
					table[i] = v;
				}
				made = true;
			}
			for (size_t i = 0; i < n; ++i)
				c = table[(c ^ p[i]) & 0xFF] ^ (c >> 8);
			return c;
		}
	}

	void ClassifyCoast(Grid<uint8_t>& land, const std::vector<std::vector<P2>>& coastlines, float reach)
	{
		Buckets b;
		const float margin = reach + 256;
		b.Build(coastlines, land.x0 - margin, land.y0 - margin, land.x0 + land.nx * land.cell + margin, land.y0 + land.ny * land.cell + margin);
		// coarse first (every 16th cell, searched as far as needed), then the cells near a coastline exactly
		constexpr int kStep = 16;
		const int maxRings = std::max(b.nx, b.ny);
		for (int j0 = 0; j0 < land.ny; j0 += kStep)
			for (int i0 = 0; i0 < land.nx; i0 += kStep)
			{
				const int ci = std::min(i0 + kStep / 2, land.nx - 1), cj = std::min(j0 + kStep / 2, land.ny - 1);
				float d = 0;
				const int side = b.Side(land.Centre(ci, cj), maxRings, d);
				const float block = kStep * land.cell * 0.75f;
				for (int j = j0; j < std::min(j0 + kStep, land.ny); ++j)
					for (int i = i0; i < std::min(i0 + kStep, land.nx); ++i)
					{
						if (side < 0)
							continue;
						if (d > block + reach)
						{
							land.At(i, j) = static_cast<uint8_t>(side);
							continue;
						}
						float dd = 0;
						const int s = b.Side(land.Centre(i, j), static_cast<int>(reach / b.size) + 2, dd);
						land.At(i, j) = static_cast<uint8_t>(s < 0 ? side : s);
					}
			}
	}

	Grid<float> DistanceTo(const Grid<uint8_t>& inside, uint8_t value)
	{
		Grid<float> d;
		d.x0 = inside.x0, d.y0 = inside.y0, d.cell = inside.cell, d.nx = inside.nx, d.ny = inside.ny;
		const float inf = 1e9f, a = inside.cell, b = inside.cell * 1.41421356f;
		d.v.resize(inside.v.size());
		for (size_t k = 0; k < inside.v.size(); ++k)
			d.v[k] = inside.v[k] == value ? inf : 0;
		for (int j = 0; j < d.ny; ++j)
			for (int i = 0; i < d.nx; ++i)
			{
				float& v = d.At(i, j);
				if (v == 0)
					continue;
				if (i > 0)
					v = std::min(v, d.At(i - 1, j) + a);
				if (j > 0)
				{
					v = std::min(v, d.At(i, j - 1) + a);
					if (i > 0)
						v = std::min(v, d.At(i - 1, j - 1) + b);
					if (i + 1 < d.nx)
						v = std::min(v, d.At(i + 1, j - 1) + b);
				}
			}
		for (int j = d.ny - 1; j >= 0; --j)
			for (int i = d.nx - 1; i >= 0; --i)
			{
				float& v = d.At(i, j);
				if (v == 0)
					continue;
				if (i + 1 < d.nx)
					v = std::min(v, d.At(i + 1, j) + a);
				if (j + 1 < d.ny)
				{
					v = std::min(v, d.At(i, j + 1) + a);
					if (i + 1 < d.nx)
						v = std::min(v, d.At(i + 1, j + 1) + b);
					if (i > 0)
						v = std::min(v, d.At(i - 1, j + 1) + b);
				}
			}
		return d;
	}

	bool WritePng(const std::filesystem::path& file, int width, int height, const std::vector<std::array<uint8_t, 3>>& rgb, std::string& error)
	{
		// rows top (north) first, each with filter 0
		loader::convert::Bytes raw;
		raw.reserve(static_cast<size_t>(height) * (width * 3 + 1));
		for (int y = height - 1; y >= 0; --y)
		{
			raw.push_back(0);
			for (int x = 0; x < width; ++x)
			{
				const auto& c = rgb[static_cast<size_t>(y) * width + x];
				raw.insert(raw.end(), c.begin(), c.end());
			}
		}
		loader::convert::Bytes deflated;
		if (!loader::convert::Deflate(raw, deflated))
			return error = "deflate failed", false;
		uint32_t s1 = 1, s2 = 0;
		for (const uint8_t c : raw)
			s1 = (s1 + c) % 65521, s2 = (s2 + s1) % 65521;
		loader::convert::Bytes z{0x78, 0x9C};
		z.insert(z.end(), deflated.begin(), deflated.end());
		const uint32_t adler = s2 << 16 | s1;
		for (int k = 3; k >= 0; --k)
			z.push_back(static_cast<uint8_t>(adler >> (8 * k)));
		std::ofstream o(file, std::ios::binary);
		if (!o)
			return error = "cannot write " + file.string(), false;
		const auto be = [](uint32_t v) {
			return std::array<uint8_t, 4>{static_cast<uint8_t>(v >> 24), static_cast<uint8_t>(v >> 16), static_cast<uint8_t>(v >> 8), static_cast<uint8_t>(v)};
		};
		const auto chunk = [&](const char* type, const loader::convert::Bytes& data) {
			const auto len = be(static_cast<uint32_t>(data.size()));
			o.write(reinterpret_cast<const char*>(len.data()), 4);
			loader::convert::Bytes body(type, type + 4);
			body.insert(body.end(), data.begin(), data.end());
			o.write(reinterpret_cast<const char*>(body.data()), static_cast<std::streamsize>(body.size()));
			const auto crc = be(Crc(body.data(), body.size()) ^ 0xFFFFFFFF);
			o.write(reinterpret_cast<const char*>(crc.data()), 4);
		};
		const uint8_t signature[8] = {0x89, 'P', 'N', 'G', '\r', '\n', 0x1A, '\n'};
		o.write(reinterpret_cast<const char*>(signature), 8);
		loader::convert::Bytes ihdr;
		for (const uint32_t v : {static_cast<uint32_t>(width), static_cast<uint32_t>(height)})
		{
			const auto b = be(v);
			ihdr.insert(ihdr.end(), b.begin(), b.end());
		}
		ihdr.insert(ihdr.end(), {8, 2, 0, 0, 0});
		chunk("IHDR", ihdr);
		chunk("IDAT", z);
		chunk("IEND", {});
		return true;
	}
}
