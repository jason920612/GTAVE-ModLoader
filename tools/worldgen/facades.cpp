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

		// lap siding: boards 18 cm to the weather, each lapped over the one below - the board thickest at its foot, a shadow
		// under that edge -, painted, a faint grain along them
		void Siding(Canvas& c, Rgb colour, uint32_t seed)
		{
			constexpr float kExposure = 0.18f;
			for (int y = 0; y < c.h; ++y)
				for (int x = 0; x < c.w; ++x)
				{
					const float u = (x + 0.5f) / c.w, v = (y + 0.5f) / c.h;
					const float up = (1 - v) * c.mh; // (metres up the wall: the image's rows run down)
					const float t = up / kExposure - std::floor(up / kExposure); // (0 at a board's foot, 1 under the next one's)
					const float grain = Fbm(u * 0.25f, v * 6.0f, 16, seed, 2);
					const float shadow = t > 0.86f ? 0.72f : 1.0f;
					const size_t i = static_cast<size_t>(y) * c.w + x;
					c.albedo[i] = Scale(colour, (0.97f + 0.06f * (1 - t) + 0.05f * grain) * shadow);
					c.height[i] = 0.62f - 0.22f * t;
					c.spec[i] = 0.12f;
				}
		}

		// brick in running bond: 21.5 x 6.5 cm and 1 cm joints - each brick its own shade, the mortar recessed
		void Brick(Canvas& c, Rgb colour, Rgb mortar, uint32_t seed)
		{
			constexpr float kLong = 0.225f, kCourse = 0.075f;
			for (int y = 0; y < c.h; ++y)
				for (int x = 0; x < c.w; ++x)
				{
					const float u = (x + 0.5f) / c.w, v = (y + 0.5f) / c.h;
					const float up = (1 - v) * c.mh, across = u * c.mw;
					const int course = static_cast<int>(std::floor(up / kCourse));
					const float along = across + (course % 2 ? kLong / 2 : 0);
					const int brick = static_cast<int>(std::floor(along / kLong));
					const float tv = up / kCourse - course, tu = along / kLong - brick;
					const size_t i = static_cast<size_t>(y) * c.w + x;
					const float fine = Fbm(u, v, 128, seed, 2);
					if (tv < 0.14f || tu < 0.046f)
					{
						c.albedo[i] = Scale(mortar, 1 + 0.1f * fine);
						c.height[i] = 0.4f;
						c.spec[i] = 0.05f;
						continue;
					}
					// (bricks of a wall differ: lighter, darker, a few nearly black from the kiln)
					const uint32_t bu = static_cast<uint32_t>(brick + 1000), bv = static_cast<uint32_t>(course + 1000);
					const float shade = Rand(bu, bv, seed) < 0.06f ? 0.62f : 0.84f + 0.32f * Rand(bu, bv, seed + 1);
					c.albedo[i] = Scale(colour, shade * (1 + 0.12f * fine));
					c.height[i] = 0.58f + 0.03f * fine;
					c.spec[i] = 0.07f;
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

	Set Make(const std::string& prefix, int size, double latitude)
	{
		Set set;
		const bool miami = latitude < 26.3;
		const auto add = [&](Kind kind, const std::string& family, const std::string& name, float w, float h, const std::function<void(Canvas&)>& draw,
		                     bool first, float bump) {
			Style s;
			s.kind = kind;
			s.width = w, s.height = h;
			s.diffuse = prefix + name;
			s.normal = prefix + family + "_n";
			s.specular = prefix + family + "_s";
			const int px = kind == Kind::Brick || kind == Kind::BrickCity ? size : size / 2; // (a siding board is 15 px even at half)
			const int detail = std::max(64, px / 4);
			Canvas c(px, px, w, h);
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
		    {"blue", {188, 208, 222}}, {"yellow", {240, 226, 160}}, {"pink", {236, 196, 200}}, {"grey", {196, 196, 190}}, {"tan", {206, 188, 156}},
		    {"green", {196, 212, 184}}, {"terracotta", {214, 160, 128}}};
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
		for (size_t k = 0; k < (miami ? std::size(deco) : 0); ++k)
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
		// parking garages: decks of 3 m - a concrete spandrel 1.1 m high, the open deck over it dark to the next slab with a
		// ceiling light every 3 m -, columns every 6 m (the street pictures' garages had come out as offices with ribbon
		// windows)
		add(Kind::Garage, "garage", "garage_grey", 12, 12, [&](Canvas& c) {
			Wall(c, {200, 200, 194}, ++seed, 0.08f, 0.14f);
			for (int f = 0; f < 4; ++f)
			{
				const float y = f * 3.0f;
				c.Rect(0, y + 1.1f, c.mw, y + 3.0f, [&](size_t i, float, float v) {
					const float shade = 30 + 16 * (1 - v);
					c.albedo[i] = {shade, shade + 1, shade + 3};
					c.height[i] = 0.12f;
					c.spec[i] = 0.04f;
				});
				for (int b = 0; b < 4; ++b)
					c.Fill(b * 3.0f + 1.2f, y + 2.86f, b * 3.0f + 1.8f, y + 2.93f, {228, 228, 210}, 0.14f, 0.3f);
				// (the slab's shadow on the spandrel's top)
				c.Rect(0, y + 0.95f, c.mw, y + 1.1f, [&](size_t i, float, float v) { c.albedo[i] = Scale(c.albedo[i], 0.8f + 0.2f * (1 - v)); });
			}
			for (const float x : {0.0f, 6.0f})
				c.Rect(x, 0, x + 0.45f, c.mh, [&](size_t i, float u, float) {
					c.albedo[i] = Scale({204, 204, 198}, 0.92f + 0.08f * u);
					c.height[i] = 0.62f;
					c.spec[i] = 0.06f;
				});
		}, true, 5);
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
		// frame houses in lap siding: two floors of 3 m, windows every 4 m in wide white trim, louvred shutters on some, a
		// block foundation
		const std::pair<const char*, Rgb> sidings[] = {{"white", {236, 236, 230}}, {"grey", {190, 196, 198}}, {"beige", {212, 198, 168}},
		    {"yellow", {232, 218, 164}}, {"blue", {176, 196, 212}}, {"sage", {172, 186, 158}}, {"tan", {194, 174, 144}}, {"cream", {230, 222, 196}},
		    {"darkgrey", {132, 138, 140}}, {"bluegrey", {150, 166, 178}}, {"green", {150, 170, 140}}, {"brown", {140, 112, 88}}, {"paleyellow", {240, 232, 196}}};
		for (size_t k = 0; k < (miami ? 0 : std::size(sidings)); ++k)
			add(Kind::Siding, "siding", std::string("siding_") + sidings[k].first, 8, 6, [&](Canvas& c) {
				Siding(c, sidings[k].second, ++seed);
				const bool shutters = k % 3 == 0; // (a third of them: the street pictures' houses mostly have none)
				const Rgb shutter = k % 2 ? Rgb{40, 52, 44} : Rgb{34, 38, 46};
				for (int f = 0; f < 2; ++f)
					for (int b = 0; b < 2; ++b)
					{
						const float x = b * 4.0f + 1.4f, y = f * 3.0f + 0.9f;
						c.Fill(x - 0.11f, y - 0.11f, x + 1.31f, y + 1.51f, {244, 244, 240}, 0.66f, 0.15f); // (the trim)
						Window(c, x, y, x + 1.2f, y + 1.4f, 0.06f, {244, 244, 240}, {80, 98, 112}, seed * 31 + f * 2 + b, true);
						if (shutters)
							for (const float sx : {x - 0.58f, x + 1.34f})
								c.Rect(sx, y - 0.05f, sx + 0.44f, y + 1.45f, [&](size_t i, float, float v) {
									// (louvres: a slat every 6 cm)
									const float slat = std::fmod(v * 1.5f / 0.06f, 1.0f);
									c.albedo[i] = Scale(shutter, 0.85f + 0.3f * slat);
									c.height[i] = 0.7f + 0.05f * slat;
									c.spec[i] = 0.12f;
								});
					}
				c.Rect(0, 0, c.mw, 0.45f, [&](size_t i, float, float) {
					c.albedo[i] = Scale(Rgb{150, 148, 142}, 0.95f + 0.1f * Rand(static_cast<uint32_t>(i), seed));
					c.height[i] = 0.7f;
					c.spec[i] = 0.05f;
				});
			}, k == 0, 5);
		// brick houses: the same floors, white-framed windows on a stone sill, a soldier course over them
		const std::pair<const char*, Rgb> bricks[] = {{"red", {150, 72, 56}}, {"brown", {128, 84, 64}}, {"orange", {172, 98, 70}}, {"buff", {190, 156, 120}}};
		for (size_t k = 0; k < (miami ? 0 : std::size(bricks)); ++k)
			add(Kind::Brick, "brick", std::string("brick_") + bricks[k].first, 8.1f, 6, [&](Canvas& c) {
				Brick(c, bricks[k].second, {196, 192, 182}, ++seed);
				for (int f = 0; f < 2; ++f)
					for (int b = 0; b < 2; ++b)
					{
						const float x = b * 4.05f + 1.4f, y = f * 3.0f + 0.9f;
						Window(c, x, y, x + 1.2f, y + 1.4f, 0.08f, {238, 238, 232}, {80, 98, 112}, seed * 31 + f * 2 + b, true);
						c.Fill(x - 0.1f, y - 0.1f, x + 1.3f, y - 0.01f, {205, 200, 188}, 0.7f, 0.08f); // (the sill)
						c.Rect(x - 0.08f, y + 1.4f, x + 1.28f, y + 1.62f, [&](size_t i, float, float) { c.albedo[i] = Scale(c.albedo[i], 0.9f); });
					}
			}, k == 0, 6);
		// red brick walk-ups and offices: floors of 3.075 m, tall windows every 2.43 m on stone sills, a band at each floor
		const std::pair<const char*, Rgb> brickCity[] = {{"red", {146, 66, 52}}, {"brown", {124, 80, 62}}, {"buff", {186, 152, 116}}};
		for (size_t k = 0; k < (miami ? 0 : std::size(brickCity)); ++k)
			add(Kind::BrickCity, "brickcity", std::string("brickcity_") + brickCity[k].first, 12.15f, 12.3f, [&](Canvas& c) {
				Brick(c, brickCity[k].second, {190, 186, 176}, ++seed);
				for (int f = 0; f < 4; ++f)
				{
					const float y = f * 3.075f;
					c.Fill(0, y + 2.9f, c.mw, y + 3.0f, {200, 194, 180}, 0.66f, 0.08f);
					for (int b = 0; b < 5; ++b)
					{
						const float x = b * 2.43f + 0.66f;
						Window(c, x, y + 0.85f, x + 1.1f, y + 2.6f, 0.07f, {226, 224, 216}, {70, 88, 102}, seed * 37 + f * 5 + b, true);
						c.Fill(x - 0.12f, y + 0.72f, x + 1.22f, y + 0.84f, {205, 200, 188}, 0.7f, 0.08f);
					}
				}
			}, k == 0, 6);
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

	int Pick(const Set& set, const std::string& kind, float height, float area, uint32_t seed, double latitude, const Seen* seen, int year)
	{
		const auto among = [&](Kind k) {
			// (siding weighted as the street pictures show the colours: white most, cream and light grey next - picked evenly
			// the reviewers had seen tan siding where most houses are white or pale)
			const auto weight = [&](const Style& st) {
				if (k != Kind::Siding)
					return 1;
				return st.diffuse.ends_with("siding_white") ? 6 : st.diffuse.ends_with("siding_cream") || st.diffuse.ends_with("siding_grey") ? 3
				       : st.diffuse.ends_with("siding_paleyellow") || st.diffuse.ends_with("siding_bluegrey") || st.diffuse.ends_with("siding_sage") ? 2
				                                                                                                                             : 1;
			};
			std::vector<int> v;
			for (size_t i = 0; i < set.styles.size(); ++i)
				if (set.styles[i].kind == k)
					for (int w = weight(set.styles[i]); w > 0; --w)
						v.push_back(static_cast<int>(i));
			return v.empty() ? 0 : v[Hash(seed) % v.size()];
		};
		const float r = Rand(seed, 99);
		// (a house's garage is the house's; the metal sheds only the warehouses, works and barns - a big store, a stadium's
		// stands, a parking garage are block and concrete in the street pictures)
		const bool house = kind == "house" || kind == "detached" || kind == "bungalow" || kind == "semidetached_house" || kind == "terrace" ||
		                   kind == "static_caravan" ||
		                   ((kind == "garage" || kind == "garages" || kind == "carport") && height < 6);
		const bool industrial = kind == "industrial" || kind == "warehouse" || kind == "shed" || kind == "hangar" || kind == "manufacture" || kind == "barn" ||
		                        kind == "farm_auxiliary" || kind == "storage_tank";
		const bool office = kind == "office" || kind == "commercial" || kind == "retail" || kind == "hospital" || kind == "university" || kind == "school";
		if (industrial)
			return among(Kind::Shed);
		if (kind == "parking" && height >= 5)
			return among(Kind::Garage);
		if (kind == "parking" || kind == "stadium" || kind == "grandstand" || kind == "garage" || kind == "garages" || (height < 10 && area > 3000 && !house))
			return among(Kind::Concrete);
		// (north Florida: frame houses in siding, brick ones, some stucco - Tallahassee's streets; south Florida: concrete
		// block and stucco, Miami's)
		const bool north = latitude >= 28.8, miami = latitude < 26.3;
		const bool apartments = kind == "apartments" || kind == "dormitory" || kind == "terrace";
		const bool homely = house || (height < 8.5f && area < 350) || (apartments && height <= 15);
		// (seen in the street pictures: brick, or a painted wall of their colour - the styles' mean colours are a little
		// darker than their paint, their windows in them; a dark colour is a wall in deep shade or behind leaves: not used)
		if (seen && !industrial)
		{
			const float lum = (seen->colour[0] + seen->colour[1] + seen->colour[2]) / 3.0f;
			const bool brick = seen->brick > 0.3f; // (a wall band is windows, trim and leaves too: a third of it brick is a brick wall)
			if (brick || lum >= 70)
			{
				std::vector<Kind> kinds;
				if (brick)
					kinds = {homely ? Kind::Brick : Kind::BrickCity};
				else if (homely)
					kinds = north || latitude >= 27.0 ? std::vector<Kind>{Kind::Siding, Kind::House} : std::vector<Kind>{Kind::House};
				else if (height < 6.5f)
					kinds = {Kind::Shop, Kind::Concrete};
				else if (height < 16)
					kinds = {Kind::Concrete, Kind::BrickCity}; // (no glass boxes: the low offices and shops are block and brick)
				else
					kinds = {Kind::Concrete, Kind::Glass, Kind::Balcony, Kind::BrickCity};
				int best = -1;
				float bestDistance = 1e30f;
				for (size_t i = 0; i < set.styles.size(); ++i)
				{
					if (std::find(kinds.begin(), kinds.end(), set.styles[i].kind) == kinds.end())
						continue;
					float distance = 0;
					for (int c = 0; c < 3; ++c)
					{
						const float diff = set.styles[i].average[c] * 1.08f - seen->colour[c];
						distance += diff * diff;
					}
					if (distance < bestDistance)
						bestDistance = distance, best = static_cast<int>(i);
				}
				if (best >= 0)
					return best;
			}
		}
		if (homely)
		{
			// (a mobile home: lap siding, the light vinyl of a single- or double-wide)
			if (kind == "static_caravan")
				return among(Kind::Siding);
			float siding = north ? 0.45f : latitude >= 27.0 ? 0.2f : 0.06f, brick = north ? 0.3f : latitude >= 27.0 ? 0.1f : 0.03f;
			// (by its era, where the rolls say when it was built: north Florida's old frame houses, its post-war brick ranches)
			if (north && year > 0)
			{
				if (year < 1940)
					siding = 0.75f, brick = 0.15f;
				else if (year < 1980)
					siding = 0.3f, brick = 0.55f;
				else if (year < 2000)
					siding = 0.5f, brick = 0.3f;
				else
					siding = 0.45f, brick = 0.25f;
			}
			return among(r < siding ? Kind::Siding : r < siding + brick ? Kind::Brick : Kind::House);
		}
		const bool retail = kind == "retail" || kind == "commercial" || kind == "supermarket" || kind == "kiosk";
		// (a third of the low shops with glass fronts all round: the street pictures' are painted block with a storefront)
		if (retail && height <= 6.5f)
			return among(r < 0.35f ? Kind::Shop : Kind::Concrete);
		if (height >= 40)
			return among(r < (office ? 0.75f : 0.45f) ? Kind::Glass : Kind::Balcony);
		if (height >= 16)
		{
			if (north)
				return among(r < 0.35f ? Kind::BrickCity : r < 0.75f ? Kind::Concrete : Kind::Glass);
			return among(office ? (r < 0.6f ? Kind::Glass : Kind::Concrete) : (r < 0.55f ? Kind::Balcony : r < 0.8f ? Kind::Concrete : Kind::Glass));
		}
		// (low and middling: red brick in the north's downtowns and campuses, Art Deco walk-ups only in Miami)
		// (a third red brick: the street pictures' institutional blocks are as often white or grey)
		if (north)
			return among(r < 0.35f ? Kind::BrickCity : Kind::Concrete);
		if (!miami)
			return among(r < 0.5f ? Kind::Concrete : r < 0.8f ? Kind::House : Kind::BrickCity);
		return among(office ? (r < 0.6f ? Kind::Concrete : Kind::Deco) : (r < 0.75f ? Kind::Deco : Kind::Concrete));
	}
}
