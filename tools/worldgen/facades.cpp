#include "facades.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <functional>

#include "texture.hpp"

namespace worldgen::facades
{
	namespace
	{
		using Rgb = std::array<float, 3>;

		uint32_t Hash(uint32_t x)
		{
			x ^= x >> 16;
			x *= 0x7feb352du;
			x ^= x >> 15;
			x *= 0x846ca68bu;
			x ^= x >> 16;
			return x;
		}

		float Rand(uint32_t a, uint32_t b = 0, uint32_t c = 0)
		{
			return static_cast<float>(Hash(a * 73856093u ^ Hash(b * 19349663u ^ Hash(c * 83492791u + 1))) & 0xFFFFFF) / 16777216.0f;
		}

		// value noise repeating every `period` cells (so the textures tile)
		float Noise(float x, float y, int period, uint32_t seed)
		{
			const int x0 = static_cast<int>(std::floor(x)), y0 = static_cast<int>(std::floor(y));
			float tx = x - x0, ty = y - y0;
			tx = tx * tx * (3 - 2 * tx), ty = ty * ty * (3 - 2 * ty);
			const auto v = [&](int i, int j) {
				return Rand(static_cast<uint32_t>(((i % period) + period) % period), static_cast<uint32_t>(((j % period) + period) % period), seed);
			};
			return (v(x0, y0) * (1 - tx) + v(x0 + 1, y0) * tx) * (1 - ty) + (v(x0, y0 + 1) * (1 - tx) + v(x0 + 1, y0 + 1) * tx) * ty;
		}

		// fractal noise over the texture (u, v in 0..1), about -0.5..0.5
		float Fbm(float u, float v, int base, uint32_t seed, int octaves = 4)
		{
			float s = 0, a = 0.5f, total = 0;
			for (int o = 0; o < octaves; ++o)
			{
				const int period = base << o;
				s += a * (Noise(u * period, v * period, period, seed + o * 101) - 0.5f);
				total += a;
				a *= 0.5f;
			}
			return s / total;
		}

		// A texture being drawn: metres across and up (y from the bottom), colour, height (for the normal map), shine.
		struct Canvas
		{
			int w, h;
			float mw, mh;
			std::vector<Rgb> albedo;
			std::vector<float> height, spec;

			Canvas(int width, int heightPx, float metresW, float metresH)
			    : w(width), h(heightPx), mw(metresW), mh(metresH), albedo(static_cast<size_t>(width) * heightPx), height(albedo.size(), 0.5f),
			      spec(albedo.size(), 0.1f)
			{
			}

			// every pixel of the rectangle (metres; wraps across the texture's edges) with its place in the rectangle
			void Rect(float x0, float y0, float x1, float y1, const std::function<void(size_t, float, float)>& f)
			{
				const int px0 = static_cast<int>(std::floor(x0 / mw * w)), px1 = static_cast<int>(std::ceil(x1 / mw * w));
				const int py0 = static_cast<int>(std::floor(y0 / mh * h)), py1 = static_cast<int>(std::ceil(y1 / mh * h));
				for (int py = py0; py < py1; ++py)
					for (int px = px0; px < px1; ++px)
					{
						const float cx = (px + 0.5f) / w * mw, cy = (py + 0.5f) / h * mh;
						if (cx < x0 || cx > x1 || cy < y0 || cy > y1)
							continue;
						const int x = ((px % w) + w) % w, y = ((py % h) + h) % h;
						// (rows of the image top down)
						f(static_cast<size_t>(h - 1 - y) * w + x, (cx - x0) / std::max(1e-4f, x1 - x0), (cy - y0) / std::max(1e-4f, y1 - y0));
					}
			}
			void Fill(float x0, float y0, float x1, float y1, Rgb c, float hgt, float sp)
			{
				Rect(x0, y0, x1, y1, [&](size_t i, float, float) { albedo[i] = c, height[i] = hgt, spec[i] = sp; });
			}
		};

		Rgb Mix(Rgb a, Rgb b, float t)
		{
			return {a[0] + (b[0] - a[0]) * t, a[1] + (b[1] - a[1]) * t, a[2] + (b[2] - a[2]) * t};
		}

		Rgb Scale(Rgb a, float s)
		{
			return {a[0] * s, a[1] * s, a[2] * s};
		}

		// plaster or concrete: the colour with fine grain, broad stains and darker streaks running down
		void Wall(Canvas& c, Rgb colour, uint32_t seed, float grain, float stains)
		{
			for (int y = 0; y < c.h; ++y)
				for (int x = 0; x < c.w; ++x)
				{
					const float u = (x + 0.5f) / c.w, v = (y + 0.5f) / c.h;
					const float fine = Fbm(u, v, 64, seed, 3), broad = Fbm(u, v, 4, seed + 7, 3);
					const float streak = Fbm(u, v * 0.15f, 24, seed + 13, 2);
					const size_t i = static_cast<size_t>(y) * c.w + x;
					c.albedo[i] = Scale(colour, 1 + grain * fine + stains * broad - 0.05f * std::max(0.0f, streak));
					c.height[i] = 0.5f + 0.04f * fine;
					c.spec[i] = 0.06f + 0.03f * fine;
				}
		}

		// glass: the sky's reflection, brighter up, a little different pane to pane
		Rgb Glass(Rgb tint, float v, float r)
		{
			const Rgb sky = Mix(Scale(tint, 0.7f), Mix(tint, Rgb{200, 215, 230}, 0.45f), v);
			return Scale(sky, 0.85f + 0.3f * r);
		}

		void Window(Canvas& c, float x0, float y0, float x1, float y1, float frame, Rgb frameColour, Rgb tint, uint32_t seed, bool crossbar)
		{
			c.Fill(x0, y0, x1, y1, frameColour, 0.56f, 0.25f);
			const float r = Rand(seed, 17);
			c.Rect(x0 + frame, y0 + frame, x1 - frame, y1 - frame, [&](size_t i, float, float v) {
				c.albedo[i] = Glass(tint, v, r);
				c.height[i] = 0.38f;
				c.spec[i] = 0.85f;
			});
			if (crossbar)
				c.Fill(x0, (y0 + y1) / 2 - frame / 2, x1, (y0 + y1) / 2 + frame / 2, frameColour, 0.52f, 0.25f);
			// (the sill's shadow under it)
			c.Rect(x0 - frame, y0 - 0.12f, x1 + frame, y0, [&](size_t i, float, float v) { c.albedo[i] = Scale(c.albedo[i], 0.75f + 0.25f * (1 - v)); });
		}

		// the canvas as the three textures
		void Finish(const Canvas& c, const std::string& diffuse, const std::string& normal, const std::string& specular, int detailSize, float bump,
		    std::vector<loader::convert::DictionaryTexture>& out)
		{
			texture::Image d(c.w, c.h);
			for (size_t i = 0; i < c.albedo.size(); ++i)
				for (int k = 0; k < 3; ++k)
					d.pixels[i][k] = static_cast<uint8_t>(std::clamp(c.albedo[i][k], 0.0f, 255.0f));
			out.push_back(texture::Make(diffuse, d, texture::Format::Bc1));
			if (normal.empty())
				return;
			// (normal and shine at a lower size: read from the canvas every few pixels)
			const int step = std::max(1, c.w / detailSize);
			const int nw = c.w / step, nh = c.h / step;
			texture::Image n(nw, nh), s(nw, nh);
			const auto H = [&](int x, int y) { return c.height[static_cast<size_t>(((y % c.h) + c.h) % c.h) * c.w + ((x % c.w) + c.w) % c.w]; };
			for (int y = 0; y < nh; ++y)
				for (int x = 0; x < nw; ++x)
				{
					const int sx = x * step, sy = y * step;
					const float dx = (H(sx + step, sy) - H(sx - step, sy)) * bump, dy = (H(sx, sy + step) - H(sx, sy - step)) * bump;
					// (image rows run down: the slope up the wall is the other way)
					const float nx = -dx, ny = dy, nz = 1;
					const float l = std::sqrt(nx * nx + ny * ny + nz * nz);
					n.At(x, y) = {static_cast<uint8_t>(std::lround((nx / l * 0.5f + 0.5f) * 255)), static_cast<uint8_t>(std::lround((ny / l * 0.5f + 0.5f) * 255)),
					    static_cast<uint8_t>(std::lround((nz / l * 0.5f + 0.5f) * 255)), 255};
					const uint8_t sv = static_cast<uint8_t>(std::clamp(c.spec[static_cast<size_t>(sy) * c.w + sx] * 255.0f, 0.0f, 255.0f));
					s.At(x, y) = {sv, sv, sv, 255};
				}
			out.push_back(texture::Make(normal, n, texture::Format::Bc1));
			out.push_back(texture::Make(specular, s, texture::Format::Bc1));
		}
	}

	Set Make(const std::string& prefix, int size)
	{
		Set set;
		const int detail = std::max(64, size / 2);
		const auto add = [&](Kind kind, const std::string& family, const std::string& name, float w, float h, const std::function<void(Canvas&)>& draw,
		                     bool first, float bump) {
			Style s;
			s.kind = kind;
			s.width = w, s.height = h;
			s.diffuse = prefix + name;
			s.normal = prefix + family + "_n";
			s.specular = prefix + family + "_s";
			Canvas c(size, size, w, h);
			draw(c);
			Finish(c, s.diffuse, first ? s.normal : std::string(), s.specular, detail, bump, set.textures);
			Rgb mean{};
			for (const Rgb& p : c.albedo)
				for (int k = 0; k < 3; ++k)
					mean[k] += p[k] / static_cast<float>(c.albedo.size());
			for (int k = 0; k < 3; ++k)
				s.average[k] = static_cast<uint8_t>(std::clamp(mean[k], 0.0f, 255.0f));
			set.styles.push_back(s);
		};
		uint32_t seed = 1;
		// houses: two floors of 3 m, a window every 4 m
		const std::pair<const char*, Rgb> houses[] = {{"white", {236, 233, 224}}, {"cream", {232, 220, 188}}, {"peach", {236, 198, 168}},
		    {"blue", {188, 208, 222}}, {"yellow", {240, 226, 160}}, {"pink", {236, 196, 200}}};
		for (size_t k = 0; k < std::size(houses); ++k)
			add(Kind::House, "house", std::string("house_") + houses[k].first, 8, 6, [&](Canvas& c) {
				Wall(c, houses[k].second, ++seed, 0.06f, 0.08f);
				for (int f = 0; f < 2; ++f)
					for (int b = 0; b < 2; ++b)
					{
						const float x = b * 4.0f + 1.4f, y = f * 3.0f + 0.9f;
						Window(c, x, y, x + 1.2f, y + 1.4f, 0.07f, {245, 245, 240}, {90, 110, 125}, seed * 31 + f * 2 + b, true);
					}
				// (a base of darker plaster)
				c.Rect(0, 0, c.mw, 0.35f, [&](size_t i, float, float) { c.albedo[i] = Scale(c.albedo[i], 0.8f); });
			}, k == 0, 6);
		// Art Deco walk-ups: floors of 3.2 m, bays of 3 m, an "eyebrow" ledge over each window, a band at each floor
		const std::pair<const char*, Rgb> deco[] = {{"mint", {178, 224, 204}}, {"pink", {240, 188, 198}}, {"lemon", {245, 230, 160}},
		    {"aqua", {160, 214, 220}}, {"white", {238, 236, 228}}, {"lilac", {205, 195, 230}}};
		for (size_t k = 0; k < std::size(deco); ++k)
			add(Kind::Deco, "deco", std::string("deco_") + deco[k].first, 12, 12.8f, [&](Canvas& c) {
				Wall(c, deco[k].second, ++seed, 0.05f, 0.08f);
				const Rgb trim = Mix(deco[k].second, {250, 250, 245}, 0.6f);
				for (int f = 0; f < 4; ++f)
				{
					c.Fill(0, f * 3.2f + 3.0f, c.mw, f * 3.2f + 3.2f, trim, 0.6f, 0.08f);
					for (int b = 0; b < 4; ++b)
					{
						const float x = b * 3.0f + 0.7f, y = f * 3.2f + 0.95f;
						Window(c, x, y, x + 1.6f, y + 1.5f, 0.06f, {240, 240, 235}, {80, 100, 118}, seed * 31 + f * 4 + b, true);
						// the ledge and its shadow
						c.Fill(x - 0.25f, y + 1.62f, x + 1.85f, y + 1.78f, trim, 0.66f, 0.08f);
						c.Rect(x - 0.25f, y + 1.3f, x + 1.85f, y + 1.62f, [&](size_t i, float, float v) { c.albedo[i] = Scale(c.albedo[i], 0.7f + 0.3f * (1 - v)); });
					}
				}
			}, k == 0, 6);
		// concrete mid-rises: floors of 3.1 m, a ribbon of windows along each
		const std::pair<const char*, Rgb> concrete[] = {{"grey", {198, 198, 192}}, {"beige", {214, 204, 184}}, {"white", {228, 228, 222}}};
		for (size_t k = 0; k < std::size(concrete); ++k)
			add(Kind::Concrete, "concrete", std::string("concrete_") + concrete[k].first, 12, 12.4f, [&](Canvas& c) {
				Wall(c, concrete[k].second, ++seed, 0.08f, 0.12f);
				for (int f = 0; f < 4; ++f)
					for (int b = 0; b < 8; ++b)
					{
						const float x = b * 1.5f, y = f * 3.1f + 1.0f;
						Window(c, x + 0.04f, y, x + 1.5f - 0.04f, y + 1.55f, 0.05f, {150, 155, 160}, {75, 95, 115}, seed * 37 + f * 8 + b, false);
					}
			}, k == 0, 5);
		// glass towers: floors of 3.4 m: 2.5 m of clear glass, 0.9 m of dark spandrel, mullions every 1.5 m
		const std::pair<const char*, Rgb> glass[] = {{"blue", {70, 108, 142}}, {"green", {72, 118, 112}}, {"grey", {66, 76, 88}}, {"silver", {122, 138, 154}}};
		for (size_t k = 0; k < std::size(glass); ++k)
			add(Kind::Glass, "glass", std::string("glass_") + glass[k].first, 9, 13.6f, [&](Canvas& c) {
				const Rgb tint = glass[k].second;
				for (int f = 0; f < 4; ++f)
				{
					const float y = f * 3.4f;
					const float r = Rand(++seed);
					c.Rect(0, y, c.mw, y + 0.9f, [&](size_t i, float, float) {
						c.albedo[i] = Scale(tint, 0.55f + 0.1f * r);
						c.height[i] = 0.45f;
						c.spec[i] = 0.7f;
					});
					for (int b = 0; b < 6; ++b)
					{
						const float pr = Rand(seed, b);
						c.Rect(b * 1.5f, y + 0.9f, b * 1.5f + 1.5f, y + 3.4f, [&](size_t i, float, float v) {
							c.albedo[i] = Glass(tint, v, pr);
							c.height[i] = 0.42f;
							c.spec[i] = 0.95f;
						});
					}
					c.Fill(0, y + 0.88f, c.mw, y + 0.96f, {190, 195, 200}, 0.55f, 0.5f);
				}
				for (int b = 0; b < 6; ++b)
					c.Fill(b * 1.5f - 0.04f, 0, b * 1.5f + 0.04f, c.mh, {195, 200, 205}, 0.58f, 0.5f);
			}, k == 0, 4);
		// white towers with balconies: slab edges, glass railings in front of glass doors
		const std::pair<const char*, Rgb> balcony[] = {{"white", {236, 236, 232}}, {"grey", {205, 207, 205}}};
		for (size_t k = 0; k < std::size(balcony); ++k)
			add(Kind::Balcony, "balcony", std::string("balcony_") + balcony[k].first, 9, 12.4f, [&](Canvas& c) {
				const Rgb slab = balcony[k].second;
				for (int f = 0; f < 4; ++f)
				{
					const float y = f * 3.1f;
					for (int b = 0; b < 3; ++b)
					{
						const float pr = Rand(++seed);
						c.Rect(b * 3.0f, y + 0.3f, b * 3.0f + 3.0f, y + 3.1f, [&](size_t i, float, float v) {
							c.albedo[i] = Scale(Glass({60, 80, 98}, v, pr), 0.8f);
							c.height[i] = 0.3f;
							c.spec[i] = 0.8f;
						});
						c.Fill(b * 3.0f - 0.12f, y + 0.3f, b * 3.0f + 0.12f, y + 3.1f, Scale(slab, 0.85f), 0.45f, 0.1f);
						// the railing: a pale glass band
						c.Rect(b * 3.0f + 0.12f, y + 0.3f, b * 3.0f + 2.88f, y + 1.35f, [&](size_t i, float, float) {
							c.albedo[i] = Mix(c.albedo[i], {175, 200, 210}, 0.55f);
							c.height[i] = 0.6f;
							c.spec[i] = 0.9f;
						});
						c.Fill(b * 3.0f + 0.12f, y + 1.33f, b * 3.0f + 2.88f, y + 1.39f, {200, 205, 208}, 0.62f, 0.6f);
					}
					c.Rect(0, y, c.mw, y + 0.3f, [&](size_t i, float, float v) {
						c.albedo[i] = Scale(slab, 0.9f + 0.1f * v);
						c.height[i] = 0.7f;
						c.spec[i] = 0.1f;
					});
				}
			}, k == 0, 5);
		// metal sheds: ribbed cladding
		const std::pair<const char*, Rgb> sheds[] = {{"grey", {188, 190, 188}}, {"beige", {210, 200, 175}}, {"blue", {150, 170, 185}}};
		for (size_t k = 0; k < std::size(sheds); ++k)
			add(Kind::Shed, "shed", std::string("shed_") + sheds[k].first, 12, 6, [&](Canvas& c) {
				Wall(c, sheds[k].second, ++seed, 0.04f, 0.15f);
				for (int y = 0; y < c.h; ++y)
					for (int x = 0; x < c.w; ++x)
					{
						const float m = (x + 0.5f) / c.w * c.mw;
						const float rib = 0.5f + 0.5f * std::cos(m / 0.25f * 6.2831853f);
						const size_t i = static_cast<size_t>(y) * c.w + x;
						c.albedo[i] = Scale(c.albedo[i], 0.88f + 0.12f * rib);
						c.height[i] = 0.4f + 0.2f * rib;
						c.spec[i] = 0.25f;
					}
			}, k == 0, 3);
		// shop fronts: 4.5 m: tall panes in aluminium, a fascia for the signs
		add(Kind::Shop, "shop", "shop", 12, 4.5f, [&](Canvas& c) {
			Wall(c, {70, 72, 76}, ++seed, 0.05f, 0.05f);
			for (int b = 0; b < 4; ++b)
				Window(c, b * 3.0f + 0.1f, 0.35f, b * 3.0f + 2.9f, 3.3f, 0.08f, {175, 178, 182}, {70, 85, 95}, seed * 41 + b, false);
			c.Fill(0, 3.45f, c.mw, 4.3f, {225, 222, 214}, 0.6f, 0.1f);
		}, true, 4);
		// flat roofs: gravel, patches of membrane
		add(Kind::Roof, "roof", "roof", 16, 16, [&](Canvas& c) {
			Wall(c, {168, 166, 160}, ++seed, 0.25f, 0.2f);
			for (int y = 0; y < c.h; ++y)
				for (int x = 0; x < c.w; ++x)
				{
					const float u = (x + 0.5f) / c.w, v = (y + 0.5f) / c.h;
					const size_t i = static_cast<size_t>(y) * c.w + x;
					if (Fbm(u, v, 3, seed + 5, 3) > 0.12f)
						c.albedo[i] = Mix(c.albedo[i], {214, 214, 210}, 0.7f);
				}
		}, true, 3);
		// the palette for the LOD models' walls
		set.palette = prefix + "lod";
		texture::Image pal(128, 8 * static_cast<int>((set.styles.size() + 15) / 16));
		for (size_t k = 0; k < set.styles.size(); ++k)
			for (int y = 0; y < 8; ++y)
				for (int x = 0; x < 8; ++x)
				{
					const auto& a = set.styles[k].average;
					pal.At(static_cast<int>(k % 16) * 8 + x, static_cast<int>(k / 16) * 8 + y) = {a[0], a[1], a[2], 255};
				}
		set.textures.push_back(texture::Make(set.palette, pal, texture::Format::Bgra));
		return set;
	}

	std::array<float, 2> Set::PaletteUv(size_t style) const
	{
		const float rows = static_cast<float>((styles.size() + 15) / 16);
		return {(static_cast<float>(style % 16) * 8 + 4) / 128.0f, (static_cast<float>(style / 16) * 8 + 4) / (8 * rows)};
	}

	int Find(const Set& set, Kind kind)
	{
		for (size_t i = 0; i < set.styles.size(); ++i)
			if (set.styles[i].kind == kind)
				return static_cast<int>(i);
		return 0;
	}

	int Pick(const Set& set, const std::string& kind, float height, float area, uint32_t seed)
	{
		const auto among = [&](Kind k) {
			std::vector<int> v;
			for (size_t i = 0; i < set.styles.size(); ++i)
				if (set.styles[i].kind == k)
					v.push_back(static_cast<int>(i));
			return v.empty() ? 0 : v[Hash(seed) % v.size()];
		};
		const float r = Rand(seed, 99);
		const bool house = kind == "house" || kind == "detached" || kind == "bungalow" || kind == "semidetached_house" || kind == "terrace";
		const bool industrial = kind == "industrial" || kind == "warehouse" || kind == "garage" || kind == "garages" || kind == "shed" || kind == "hangar";
		const bool office = kind == "office" || kind == "commercial" || kind == "retail" || kind == "hospital" || kind == "university" || kind == "school";
		if (industrial || (height < 10 && area > 3000 && !house))
			return among(Kind::Shed);
		if (house || (height < 8.5f && area < 350))
			return among(Kind::House);
		if (height >= 40)
			return among(r < (office ? 0.75f : 0.45f) ? Kind::Glass : Kind::Balcony);
		if (height >= 16)
			return among(office ? (r < 0.6f ? Kind::Glass : Kind::Concrete) : (r < 0.55f ? Kind::Balcony : r < 0.8f ? Kind::Concrete : Kind::Glass));
		return among(office ? (r < 0.6f ? Kind::Concrete : Kind::Deco) : (r < 0.75f ? Kind::Deco : Kind::Concrete));
	}
}
