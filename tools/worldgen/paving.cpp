#include "paving.hpp"

#include <algorithm>
#include <cmath>

namespace worldgen::paving
{
	namespace
	{
		uint32_t Mix(uint32_t h)
		{
			h ^= h >> 16;
			h *= 0x7feb352d;
			h ^= h >> 15;
			h *= 0x846ca68b;
			return h ^ (h >> 16);
		}

		// A deterministic stream of numbers in [0, 1).
		struct Random
		{
			uint32_t state;
			float Next()
			{
				state = Mix(state + 0x9e3779b9u);
				return static_cast<float>(state >> 8) / 16777216.0f;
			}
			float Between(float a, float b) { return a + (b - a) * Next(); }
		};

		float Smooth(float t)
		{
			t = std::clamp(t, 0.0f, 1.0f);
			return t * t * (3 - 2 * t);
		}

		// Value noise in [-1, 1] on a lattice of `cell` metres, smoothly interpolated.
		float Noise(uint32_t seed, float x, float y, float cell)
		{
			const float fx = x / cell, fy = y / cell;
			const int ix = static_cast<int>(std::floor(fx)), iy = static_cast<int>(std::floor(fy));
			const float tx = Smooth(fx - ix), ty = Smooth(fy - iy);
			const auto at = [&](int i, int j) {
				return static_cast<float>(Mix(seed ^ Mix(static_cast<uint32_t>(i) * 73856093u ^ static_cast<uint32_t>(j) * 19349663u)) >> 8) / 8388608.0f - 1.0f;
			};
			const float a = at(ix, iy), b = at(ix + 1, iy), c = at(ix, iy + 1), d = at(ix + 1, iy + 1);
			return (a + (b - a) * tx) * (1 - ty) + (c + (d - c) * tx) * ty;
		}

		bool Overlaps(const std::vector<Feature>& list, const Feature& f, float margin)
		{
			return std::any_of(list.begin(), list.end(), [&](const Feature& g) {
				return f.s0 < g.s1 + margin && g.s0 < f.s1 + margin && f.o0 < g.o1 + margin && g.o0 < f.o1 + margin;
			});
		}

		// a pothole's bowl: q 0 at its middle, 1 at its rim
		float Bowl(const Feature& f, float s, float o)
		{
			const float ra = (f.s1 - f.s0) / 2.8f, rb = (f.o1 - f.o0) / 2.8f;
			const float ds = (s - (f.s0 + f.s1) / 2) / ra, dq = (o - (f.o0 + f.o1) / 2) / rb;
			return ds * ds + dq * dq;
		}
	}

	Paving Of(const std::string& surface)
	{
		if (surface.starts_with("concrete"))
			return Paving::Concrete;
		if (surface == "brick" || surface == "bricks" || surface == "clay")
			return Paving::Bricks;
		if (surface == "sett" || surface.starts_with("cobblestone") || surface == "unhewn_cobblestone")
			return Paving::Setts;
		if (surface.starts_with("paving_stones") || surface == "stone")
			return Paving::Slabs;
		if (surface == "gravel" || surface == "fine_gravel" || surface == "compacted" || surface == "pebblestone" || surface == "unpaved" || surface == "rock")
			return Paving::Gravel;
		if (surface == "dirt" || surface == "earth" || surface == "ground" || surface == "mud" || surface == "grass" || surface == "grass_paver")
			return Paving::Dirt;
		if (surface == "sand")
			return Paving::Sand;
		return Paving::Asphalt; // (asphalt, paved, chipseal, untagged)
	}

	uint8_t Material(Paving p)
	{
		// TARMAC, CONCRETE, BRICK_PAVEMENT, COBBLESTONE, PAVING_SLAB, GRAVEL_SMALL, DIRT_TRACK, SAND_TRACK
		constexpr uint8_t kRows[kPavings] = {4, 1, 175, 12, 15, 31, 35, 21};
		return kRows[static_cast<size_t>(p)];
	}

	uint8_t BrokenMaterial(Paving p)
	{
		// TARMAC_POTHOLE, CONCRETE_POTHOLE (both rumble), the pavers as they are, GRAVEL_LARGE, MUD_POTHOLE
		constexpr uint8_t kRows[kPavings] = {6, 2, 175, 12, 15, 32, 37, 21};
		return kRows[static_cast<size_t>(p)];
	}

	float TextureMetres(Paving p)
	{
		constexpr float kMetres[kPavings] = {6.0f, 7.4f, 1.6f, 2.0f, 3.0f, 4.0f, 6.0f, 6.0f};
		return kMetres[static_cast<size_t>(p)];
	}

	bool Marked(Paving p)
	{
		return p == Paving::Asphalt || p == Paving::Concrete;
	}

	bool Rough(Paving p)
	{
		return p == Paving::Gravel || p == Paving::Dirt || p == Paving::Sand;
	}

	float Wear(const std::string& kind, const std::string& smoothness, int64_t id)
	{
		if (smoothness == "excellent")
			return 0.05f;
		if (smoothness == "good")
			return 0.2f;
		if (smoothness == "intermediate")
			return 0.45f;
		if (smoothness == "bad")
			return 0.7f;
		if (smoothness == "very_bad")
			return 0.85f;
		if (smoothness == "horrible" || smoothness == "very_horrible")
			return 0.94f;
		if (smoothness == "impassable")
			return 1.0f;
		const std::string base = kind.ends_with("_link") ? kind.substr(0, kind.size() - 5) : kind;
		const float typical = base == "motorway" || base == "trunk" ? 0.12f : base == "primary" ? 0.18f : base == "secondary" ? 0.24f : base == "tertiary" ? 0.3f
		                    : base == "service"                                                                                         ? 0.5f
		                                                                                                                                 : 0.36f;
		Random r{Mix(static_cast<uint32_t>(id) ^ Mix(static_cast<uint32_t>(id >> 32)))};
		return std::clamp(typical + (r.Next() - 0.5f) * 0.3f, 0.0f, 1.0f);
	}

	std::vector<Feature> Features(Paving p, float wear, uint32_t seed, float s0, float s1, float half, const std::vector<float>& lines, bool edgeLines)
	{
		std::vector<Feature> out;
		const float length = s1 - s0;
		if (length < 3 || half < 1.2f)
			return out;
		Random r{Mix(seed)};
		const bool paved = p == Paving::Asphalt || p == Paving::Concrete;
		const auto count = [&](float perHundred) { return static_cast<int>(std::floor(std::max(0.0f, perHundred) * length / 100 + r.Next())); };
		const auto clearOfLines = [&](float o0, float o1) {
			return std::none_of(lines.begin(), lines.end(), [&](float l) { return l > o0 - 0.15f && l < o1 + 0.15f; });
		};
		// potholes: in the wheel paths (anywhere across but clear of the painted lines), bigger and deeper the more worn
		const int potholes = paved ? count((wear - 0.35f) * (p == Paving::Asphalt ? 8.0f : 5.0f)) : Rough(p) ? count((0.3f + wear) * 6.0f) : 0;
		for (int k = 0; k < potholes; ++k)
			for (int attempt = 0; attempt < 8; ++attempt)
			{
				const float grow = 0.7f + wear;
				const float ra = r.Between(0.22f, 0.5f) * grow, rb = r.Between(0.18f, 0.4f) * grow;
				const float s = r.Between(s0 + ra * 1.4f, s1 - ra * 1.4f), o = r.Between(-half + 0.35f + rb * 1.4f, half - 0.35f - rb * 1.4f);
				Feature f{Feature::Kind::Pothole, s - ra * 1.4f, s + ra * 1.4f, o - rb * 1.4f, o + rb * 1.4f, 0, 0};
				f.depth = Rough(p) ? r.Between(0.06f, 0.15f) : 0.035f + r.Between(0.0f, 0.09f) * wear;
				if (f.s0 < s0 || f.s1 > s1 || f.o0 < -half + 0.3f || f.o1 > half - 0.3f || !clearOfLines(f.o0, f.o1) || Overlaps(out, f, 0.3f))
					continue;
				out.push_back(f);
				break;
			}
		if (!paved)
			return out;
		// patches (utility cuts, filled holes): flush, anywhere
		const int patches = count(wear * 3.0f);
		for (int k = 0; k < patches; ++k)
			for (int attempt = 0; attempt < 8; ++attempt)
			{
				const uint8_t variant = static_cast<uint8_t>(r.Next() * 3);
				const float along = variant == 0 ? r.Between(2.4f, 3.2f) : variant == 1 ? r.Between(1.0f, 1.4f) : r.Between(1.4f, 1.8f);
				const float across = variant == 0 ? r.Between(0.6f, 0.8f) : variant == 1 ? along * r.Between(0.9f, 1.1f) : r.Between(1.2f, 1.6f);
				const float s = r.Between(s0 + along / 2, s1 - along / 2), o = r.Between(-half + 0.4f + across / 2, half - 0.4f - across / 2);
				Feature f{Feature::Kind::Patch, s - along / 2, s + along / 2, o - across / 2, o + across / 2, 0, variant};
				if (f.s0 < s0 || f.s1 > s1 || f.o0 < -half + 0.35f || f.o1 > half - 0.35f || Overlaps(out, f, 0.3f))
					continue;
				out.push_back(f);
				break;
			}
		// broken edges along the kerbs (where no edge line is painted): crumbled asphalt a little lower
		if (p == Paving::Asphalt && !edgeLines && half > 2.5f)
			for (const float side : {-1.0f, 1.0f})
			{
				const int n = count((wear - 0.45f) * 4.0f);
				for (int k = 0; k < n; ++k)
					for (int attempt = 0; attempt < 6; ++attempt)
					{
						const float along = r.Between(2.5f, 6.0f), across = r.Between(0.7f, 1.1f);
						const float s = r.Between(s0 + along / 2, s1 - along / 2);
						Feature f{Feature::Kind::Breakup, s - along / 2, s + along / 2, side < 0 ? -half : half - across, side < 0 ? -half + across : half, 0, 0};
						f.depth = r.Between(0.02f, 0.035f);
						if (f.s0 < s0 || f.s1 > s1 || !clearOfLines(f.o0 + (side < 0 ? 0.3f : 0.0f), f.o1 - (side > 0 ? 0.3f : 0.0f)) || Overlaps(out, f, 0.5f))
							continue;
						out.push_back(f);
						break;
					}
			}
		return out;
	}

	float Relief(Paving p, const std::vector<Feature>& features, uint32_t seed, float s, float o, float half, float sStart, float sEnd)
	{
		// (nothing at the kerbs and where the junctions join)
		const float margin = Smooth((half - std::fabs(o)) / 0.25f) * Smooth((s - sStart) / 1.0f) * Smooth((sEnd - s) / 1.0f);
		if (margin <= 0)
			return 0;
		float r = 0;
		for (const Feature& f : features)
		{
			if (f.kind == Feature::Kind::Patch || s < f.s0 || s > f.s1 || o < f.o0 || o > f.o1)
				continue;
			if (f.kind == Feature::Kind::Pothole)
			{
				// steep-sided, a flattish broken bottom
				const float q = Bowl(f, s, o);
				if (q < 1)
					r -= f.depth * (1 - q * q) * (0.85f + 0.15f * Noise(seed ^ 0x51u, s, o, 0.15f));
			}
			else
			{
				// the edge crumbled away: lower over the box, eased in along it and towards the road's middle
				const bool right = f.o0 <= -half + 1e-3f;
				const float inward = right ? f.o1 - o : o - f.o0;
				r -= f.depth * Smooth((s - f.s0) / 0.6f) * Smooth((f.s1 - s) / 0.6f) * Smooth(inward / 0.3f);
			}
		}
		if (Rough(p))
		{
			// unpaved: bumps of a few centimetres (shorter and longer waves), on dirt and sand ruts in the wheel tracks
			const float bumps = p == Paving::Gravel ? 0.025f : 0.035f, swells = p == Paving::Gravel ? 0.04f : 0.06f;
			r += bumps * Noise(seed, s, o, 4.0f) + swells * Noise(seed ^ 0x9e37u, s, o, 9.0f);
			if (p != Paving::Gravel)
				for (const float track : {-half / 2 - 0.8f, -half / 2 + 0.8f, half / 2 - 0.8f, half / 2 + 0.8f})
				{
					const float d = (o - track) / 0.3f;
					if (std::fabs(d) < 1)
						r -= 0.04f * (1 - d * d) * (0.7f + 0.3f * Noise(seed ^ 0x77u, s, track, 3.0f));
				}
		}
		return r * margin;
	}

	const Feature* BrokenAt(const std::vector<Feature>& features, float s, float o)
	{
		for (const Feature& f : features)
		{
			if (f.kind == Feature::Kind::Patch || s < f.s0 || s > f.s1 || o < f.o0 || o > f.o1)
				continue;
			if (f.kind == Feature::Kind::Breakup || Bowl(f, s, o) < 1)
				return &f;
		}
		return nullptr;
	}

	void Picture(const Feature& f, float& u0, float& v0, float& u1, float& v1)
	{
		switch (f.kind)
		{
		case Feature::Kind::Pothole:
			// (et_pothole1: the hole in the middle, broken asphalt round it)
			u0 = 0.08f, u1 = 0.92f, v0 = 0.15f, v1 = 0.85f;
			return;
		case Feature::Kind::Patch:
			// (worldgen's own patch picture, whole: mirrored one way or the other for variety)
			u0 = f.variant == 1 ? 1.0f : 0.0f, u1 = 1 - u0;
			v0 = f.variant == 2 ? 1.0f : 0.0f, v1 = 1 - v0;
			return;
		case Feature::Kind::Breakup:
			// (im_road_damage_05: crumbled asphalt; its right quarter is brick, left out)
			u0 = 0.0f, u1 = 0.72f, v0 = 0.0f, v1 = 1.0f;
			return;
		}
	}

	float Fade(const Feature& f, float s, float o, float half)
	{
		switch (f.kind)
		{
		case Feature::Kind::Pothole:
			return std::clamp(1 - (std::sqrt(Bowl(f, s, o)) - 1) / 0.4f, 0.0f, 1.0f);
		case Feature::Kind::Breakup:
		{
			const bool right = f.o0 <= -half + 1e-3f;
			const float inward = right ? f.o1 - o : o - f.o0;
			return Smooth((s - f.s0) / 0.8f) * Smooth((f.s1 - s) / 0.8f) * Smooth(inward / 0.35f);
		}
		default:
			return 1;
		}
	}
}
