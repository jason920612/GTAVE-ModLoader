#include "elevation.hpp"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <limits>

namespace worldgen::elevation
{
	namespace
	{
		struct Header
		{
			char magic[4];
			uint32_t version;
			float x0, y0, cell;
			uint32_t width, height, layers;
		};
		static_assert(sizeof(Header) == 32);

		bool ReadHeader(std::ifstream& in, const char* magic, Header& h)
		{
			return in.read(reinterpret_cast<char*>(&h), sizeof(h)) && std::memcmp(h.magic, magic, 4) == 0 && h.width == h.height && h.width > 0 &&
			       h.width <= 8192;
		}

		template <typename T>
		bool ReadLayer(std::ifstream& in, std::vector<T>& v, size_t n)
		{
			v.resize(n);
			return static_cast<bool>(in.read(reinterpret_cast<char*>(v.data()), static_cast<std::streamsize>(n * sizeof(T))));
		}
	}

	bool ScanTile::Load(const std::filesystem::path& file)
	{
		std::ifstream in(file, std::ios::binary);
		Header h;
		if (!in || !ReadHeader(in, "WBLR", h) || h.version != 2 || h.layers != 5)
			return false;
		x0 = h.x0, y0 = h.y0, cell = h.cell, n = static_cast<int>(h.width);
		const size_t cells = static_cast<size_t>(n) * n;
		return ReadLayer(in, top, cells) && ReadLayer(in, ground, cells) && ReadLayer(in, roof, cells) && ReadLayer(in, count, cells) &&
		       ReadLayer(in, flags, cells);
	}

	bool AerialTile::Load(const std::filesystem::path& file)
	{
		std::ifstream in(file, std::ios::binary);
		Header h;
		if (!in || !ReadHeader(in, "WBAI", h) || h.version != 1 || h.layers != 4)
			return false;
		x0 = h.x0, y0 = h.y0, cell = h.cell, n = static_cast<int>(h.width);
		return ReadLayer(in, pixels, static_cast<size_t>(n) * n);
	}

	template <typename Tile>
	bool Tiles<Tile>::Open(const std::filesystem::path& folder, const char* prefix, size_t keep)
	{
		names_.clear();
		loaded_.clear();
		order_.clear();
		keep_ = keep;
		std::error_code ec;
		if (folder.empty() || !std::filesystem::is_directory(folder, ec))
			return false;
		for (const auto& e : std::filesystem::directory_iterator(folder, ec))
		{
			const std::string name = e.path().filename().string();
			if (!name.starts_with(prefix) || !name.ends_with(".bin"))
				continue;
			int i = 0, j = 0;
			if (sscanf_s(name.c_str() + std::strlen(prefix), "%d_%d", &i, &j) == 2)
				names_[{i, j}] = e.path();
		}
		if (names_.empty())
			return false;
		// the tiles' size and cells from the first one
		Tile t;
		if (!t.Load(names_.begin()->second))
			return names_.clear(), false;
		cell_ = t.cell, n_ = t.n, size_ = t.cell * t.n;
		return true;
	}

	template <typename Tile>
	const Tile* Tiles<Tile>::At(float x, float y, int& i, int& j)
	{
		const int ti = static_cast<int>(std::floor(x / size_)), tj = static_cast<int>(std::floor(y / size_));
		const std::pair<int, int> key{ti, tj};
		auto it = loaded_.find(key);
		if (it == loaded_.end())
		{
			std::unique_ptr<Tile> t;
			if (const auto n = names_.find(key); n != names_.end())
			{
				t = std::make_unique<Tile>();
				if (!t->Load(n->second))
					t.reset();
			}
			order_.push_front(key);
			it = loaded_.emplace(key, std::make_pair(std::move(t), order_.begin())).first;
			while (loaded_.size() > keep_)
			{
				loaded_.erase(order_.back());
				order_.pop_back();
			}
		}
		else if (it->second.second != order_.begin())
		{
			order_.splice(order_.begin(), order_, it->second.second);
			it->second.second = order_.begin();
		}
		const Tile* t = it->second.first.get();
		if (!t)
			return nullptr;
		i = std::clamp(static_cast<int>((x - t->x0) / t->cell), 0, t->n - 1);
		j = std::clamp(static_cast<int>((y - t->y0) / t->cell), 0, t->n - 1);
		return t;
	}

	template class Tiles<ScanTile>;
	template class Tiles<AerialTile>;

	float Scan::Value(float x, float y, std::vector<int16_t> ScanTile::*layer)
	{
		int i = 0, j = 0;
		const ScanTile* t = tiles_.At(x, y, i, j);
		if (!t)
			return std::numeric_limits<float>::quiet_NaN();
		const int16_t v = (t->*layer)[static_cast<size_t>(j) * t->n + i];
		return v == -32768 ? std::numeric_limits<float>::quiet_NaN() : v * 0.01f + offset_;
	}

	uint8_t Scan::Flags(float x, float y)
	{
		int i = 0, j = 0;
		const ScanTile* t = tiles_.At(x, y, i, j);
		return t ? t->flags[static_cast<size_t>(j) * t->n + i] : 0;
	}

	uint8_t Scan::Count(float x, float y)
	{
		int i = 0, j = 0;
		const ScanTile* t = tiles_.At(x, y, i, j);
		return t ? t->count[static_cast<size_t>(j) * t->n + i] : 0;
	}

	bool Aerial::Sample(float x, float y, std::array<float, 4>& out)
	{
		// the four pixel centres around (x, y) (each looked up on its own: they may lie in neighbouring tiles)
		const float c = 0.25f;
		const float fx = x / c - 0.5f, fy = y / c - 0.5f;
		const float bx = std::floor(fx), by = std::floor(fy);
		const float tx = fx - bx, ty = fy - by;
		out = {};
		float weight = 0;
		for (int k = 0; k < 4; ++k)
		{
			const float px = (bx + (k & 1) + 0.5f) * c, py = (by + (k >> 1) + 0.5f) * c;
			int i = 0, j = 0;
			const AerialTile* t = tiles_.At(px, py, i, j);
			if (!t)
				continue;
			const auto& p = t->pixels[static_cast<size_t>(j) * t->n + i];
			if (!p[0] && !p[1] && !p[2])
				continue;
			const float w = ((k & 1) ? tx : 1 - tx) * ((k >> 1) ? ty : 1 - ty);
			for (int b = 0; b < 4; ++b)
				out[b] += w * p[b];
			weight += w;
		}
		if (weight < 1e-4f)
			return false;
		for (float& v : out)
			v /= weight;
		return true;
	}
}
