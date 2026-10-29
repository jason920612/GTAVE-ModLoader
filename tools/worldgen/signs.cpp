#include "signs.hpp"

#include <Windows.h>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <map>
#include <unordered_map>

#include "convert/resource.hpp"

namespace worldgen::signs
{
	namespace
	{
		using P2 = std::array<float, 2>;
		using P3 = std::array<float, 3>;
		using Uv = std::array<float, 2>;

		constexpr uint8_t kRailing = 65; // (materials.dat: the posts' metal, as the barriers' railings)
		constexpr uint8_t kCollisionKind = 3; // (roads::Collision's kinds: in the way, never walked on - the barriers')

		// The glyphs drawn by the system's font renderer (GDI) at 44 pixels to the em into cells of 64: chars 32-126 in
		// rows of 16, char 127's cell solid white. Bahnschrift (DIN, as condensed as the highway alphabets) where the
		// system has it.
		struct Font
		{
			static constexpr int kCell = 64, kColumns = 16, kWidth = 1024, kHeight = 512, kEm = 44, kPadX = 8, kPadY = 2;
			int ascent = 0, cap = 0;      // (pixels: the baseline's depth in a cell from kPadY, the capitals' height)
			std::array<float, 96> advance{}; // (pixels, chars 32-127)
			texture::Image picture;
		};

		const Font& TheFont()
		{
			static const Font font = [] {
				Font f;
				f.picture = texture::Image(Font::kWidth, Font::kHeight, {255, 255, 255, 0});
				HDC dc = CreateCompatibleDC(nullptr);
				BITMAPINFO bi{};
				bi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
				bi.bmiHeader.biWidth = Font::kWidth;
				bi.bmiHeader.biHeight = -Font::kHeight;
				bi.bmiHeader.biPlanes = 1;
				bi.bmiHeader.biBitCount = 32;
				bi.bmiHeader.biCompression = BI_RGB;
				void* bits = nullptr;
				HBITMAP bitmap = CreateDIBSection(dc, &bi, DIB_RGB_COLORS, &bits, nullptr, 0);
				if (!dc || !bitmap || !bits)
					return f;
				const HGDIOBJ oldBitmap = SelectObject(dc, bitmap);
				HFONT chosen = nullptr;
				HGDIOBJ oldFont = nullptr;
				for (const wchar_t* face : {L"Bahnschrift SemiBold SemiConden", L"Bahnschrift SemiBold", L"Bahnschrift", L"Arial"})
				{
					HFONT candidate = CreateFontW(-Font::kEm, 0, 0, 0, FW_SEMIBOLD, FALSE, FALSE, FALSE, DEFAULT_CHARSET, OUT_TT_PRECIS, CLIP_DEFAULT_PRECIS,
					    ANTIALIASED_QUALITY, DEFAULT_PITCH | FF_DONTCARE, face);
					const HGDIOBJ previous = SelectObject(dc, candidate);
					wchar_t got[64]{};
					GetTextFaceW(dc, 64, got);
					if (_wcsicmp(got, face) == 0)
					{
						chosen = candidate, oldFont = previous;
						break;
					}
					SelectObject(dc, previous);
					DeleteObject(candidate);
				}
				if (chosen)
				{
					SetBkMode(dc, TRANSPARENT);
					SetTextColor(dc, RGB(255, 255, 255));
					SetTextAlign(dc, TA_TOP | TA_LEFT);
					TEXTMETRICW tm{};
					GetTextMetricsW(dc, &tm);
					f.ascent = tm.tmAscent;
					GLYPHMETRICS gm{};
					const MAT2 identity{{0, 1}, {0, 0}, {0, 0}, {0, 1}};
					f.cap = GetGlyphOutlineW(dc, L'H', GGO_METRICS, &gm, 0, nullptr, &identity) != GDI_ERROR ? static_cast<int>(gm.gmBlackBoxY)
					                                                                                           : tm.tmAscent * 7 / 10;
					for (wchar_t c = 32; c < 127; ++c)
					{
						ABC abc{};
						if (GetCharABCWidthsW(dc, c, c, &abc))
							f.advance[c - 32] = static_cast<float>(abc.abcA + static_cast<int>(abc.abcB) + abc.abcC);
						const int col = (c - 32) % Font::kColumns, row = (c - 32) / Font::kColumns;
						TextOutW(dc, col * Font::kCell + Font::kPadX, row * Font::kCell + Font::kPadY, &c, 1);
					}
					GdiFlush();
					const auto* px = static_cast<const uint8_t*>(bits);
					for (int y = 0; y < Font::kHeight; ++y)
						for (int x = 0; x < Font::kWidth; ++x)
							f.picture.At(x, y)[3] = px[(static_cast<size_t>(y) * Font::kWidth + x) * 4 + 1]; // (grey antialiasing: any channel the coverage)
					SelectObject(dc, oldFont);
					DeleteObject(chosen);
				}
				// (char 127: the borders' solid white)
				const int col = 95 % Font::kColumns, row = 95 / Font::kColumns;
				for (int y = row * Font::kCell; y < (row + 1) * Font::kCell; ++y)
					for (int x = col * Font::kCell; x < (col + 1) * Font::kCell; ++x)
						f.picture.At(x, y)[3] = 255;
				SelectObject(dc, oldBitmap);
				DeleteObject(bitmap);
				DeleteDC(dc);
				return f;
			}();
			return font;
		}

		// the street kinds that have signs, and how they rank (the least's drivers stop; the bigger the street, the
		// bigger its blade)
		int Rank(const roads::Edge& e)
		{
			if (e.kind.ends_with("_link") || e.kind.starts_with("motorway"))
				return -1;
			if (e.kind == "trunk")
				return 5;
			if (e.kind == "primary")
				return 4;
			if (e.kind == "secondary")
				return 3;
			if (e.kind == "tertiary")
				return 2;
			if (e.kind == "residential" || e.kind == "unclassified" || e.kind == "living_street")
				return 1;
			return -1;
		}

		P2 Left(const P2& d) { return {-d[1], d[0]}; }

		uint32_t Add(roads::Mesh& m, const P3& p, const P3& n, const P2& t, const Uv& uv)
		{
			ydr::Vertex v;
			v.position = p;
			v.normal = n;
			v.tangent = {t[0], t[1], 0, 1};
			v.colour0 = {255, 255, 255, 255};
			v.uv[0] = uv;
			m.vertices.push_back(v);
			return static_cast<uint32_t>(m.vertices.size() - 1);
		}
		// a vertical quad facing `n`: from `a` along `r` (its reading way, as seen from the side it faces) `w` metres, from z0
		// up to z1; its picture's corners u0..u1, v1 (bottom)..v0 (top)
		void Quad(roads::Mesh& m, const P2& a, const P2& r, float w, float z0, float z1, const P2& n, float u0, float u1, float v0, float v1)
		{
			const P2 b{a[0] + r[0] * w, a[1] + r[1] * w};
			const P3 nn{n[0], n[1], 0};
			const uint32_t i0 = Add(m, {a[0], a[1], z0}, nn, r, {u0, v1}), i1 = Add(m, {b[0], b[1], z0}, nn, r, {u1, v1}), i2 = Add(m, {b[0], b[1], z1}, nn, r, {u1, v0}),
			               i3 = Add(m, {a[0], a[1], z1}, nn, r, {u0, v0});
			m.indices.insert(m.indices.end(), {i0, i1, i2, i0, i2, i3});
		}
		// a horizontal quad (the blade's top or bottom): round c, `half` along d, `t` across, at z, facing up or down
		void Flat(roads::Mesh& m, const P2& c, const P2& d, float half, float t, float z, bool up)
		{
			const P2 n = Left(d);
			const P2 a{c[0] - d[0] * half - n[0] * t, c[1] - d[1] * half - n[1] * t}, b{c[0] + d[0] * half - n[0] * t, c[1] + d[1] * half - n[1] * t},
			    e{c[0] + d[0] * half + n[0] * t, c[1] + d[1] * half + n[1] * t}, g{c[0] - d[0] * half + n[0] * t, c[1] - d[1] * half + n[1] * t};
			const P3 nn{0, 0, up ? 1.0f : -1.0f};
			const uint32_t i0 = Add(m, {a[0], a[1], z}, nn, d, {0.5f, 0.5f}), i1 = Add(m, {b[0], b[1], z}, nn, d, {0.5f, 0.5f}),
			               i2 = Add(m, {e[0], e[1], z}, nn, d, {0.5f, 0.5f}), i3 = Add(m, {g[0], g[1], z}, nn, d, {0.5f, 0.5f});
			if (up)
				m.indices.insert(m.indices.end(), {i0, i1, i2, i0, i2, i3});
			else
				m.indices.insert(m.indices.end(), {i0, i2, i1, i0, i3, i2});
		}

		// A blade at c (its middle; zc its height's middle) along d - its long way the street's -, `hb` high, the text on both
		// faces (each read left to right from its side), a white border inside the edge.
		void Blade(Meshes& out, const Font& f, const std::string& text, const P2& c, float zc, const P2& d, float hb)
		{
			// (capitals 0.62 of the blade's height: 6 inch blades, 4 inch capitals; the margins 0.35 of it at each end)
			const float s = 0.62f * hb / static_cast<float>(std::max(1, f.cap));
			float textPx = 0;
			for (const char ch : text)
				textPx += f.advance[static_cast<size_t>(std::clamp(static_cast<int>(static_cast<unsigned char>(ch)), 32, 126) - 32)];
			const float margin = 0.35f * hb;
			float sx = s, textW = textPx * s, length = std::max(0.45f, textW + 2 * margin);
			// (a name longer than the longest blade, 1.6 m: its letters narrowed)
			if (length > 1.6f)
				sx = s * (1.6f - 2 * margin) / textW, textW = 1.6f - 2 * margin, length = 1.6f;
			const float half = length / 2, t = 0.006f, z0 = zc - hb / 2, z1 = zc + hb / 2;
			const P2 n = Left(d);
			const float wu = (static_cast<float>(95 % Font::kColumns) + 0.5f) * Font::kCell / Font::kWidth,
			            wv = (static_cast<float>(95 / Font::kColumns) + 0.5f) * Font::kCell / Font::kHeight;
			for (const float side : {1.0f, -1.0f})
			{
				const P2 nf{n[0] * side, n[1] * side}, r{-nf[1], nf[0]};
				// the green face, its whole width
				const P2 corner{c[0] + nf[0] * t - r[0] * half, c[1] + nf[1] * t - r[1] * half};
				Quad(out.blades, corner, r, length, z0, z1, nf, 0.5f, 0.5f, 0.5f, 0.5f);
				// the end facing the way it reads: the blade's thickness
				const P2 end{c[0] + r[0] * half + nf[0] * t, c[1] + r[1] * half + nf[1] * t};
				Quad(out.blades, end, P2{-nf[0], -nf[1]}, 2 * t, z0, z1, r, 0.5f, 0.5f, 0.5f, 0.5f);
				// the border and the letters 2 mm off the face
				const P2 face{c[0] + nf[0] * (t + 0.002f), c[1] + nf[1] * (t + 0.002f)};
				const float e = 0.04f * hb, bw = 0.06f * hb, inner = half - e;
				const P2 left{face[0] - r[0] * inner, face[1] - r[1] * inner};
				Quad(out.letters, left, r, 2 * inner, z1 - e - bw, z1 - e, nf, wu, wu, wv, wv);
				Quad(out.letters, left, r, 2 * inner, z0 + e, z0 + e + bw, nf, wu, wu, wv, wv);
				Quad(out.letters, left, r, bw, z0 + e + bw, z1 - e - bw, nf, wu, wu, wv, wv);
				const P2 right{face[0] + r[0] * (inner - bw), face[1] + r[1] * (inner - bw)};
				Quad(out.letters, right, r, bw, z0 + e + bw, z1 - e - bw, nf, wu, wu, wv, wv);
				// (the capitals' middle on the blade's: each glyph's cell from the pen less its pad, its top the pad and
				// the ascent over the baseline)
				const float baseline = zc - f.cap * s / 2, top = baseline + (Font::kPadY + f.ascent) * s, bottom = top - Font::kCell * s;
				float pen = 0;
				for (const char ch : text)
				{
					const int k = std::clamp(static_cast<int>(static_cast<unsigned char>(ch)), 32, 126) - 32;
					if (k > 0)
					{
						const P2 at{face[0] + r[0] * (-textW / 2 + (pen - Font::kPadX) * sx), face[1] + r[1] * (-textW / 2 + (pen - Font::kPadX) * sx)};
						const float u0 = static_cast<float>(k % Font::kColumns) * Font::kCell / Font::kWidth, v0 = static_cast<float>(k / Font::kColumns) * Font::kCell / Font::kHeight;
						Quad(out.letters, at, r, Font::kCell * sx, bottom, top, nf, u0, u0 + static_cast<float>(Font::kCell) / Font::kWidth, v0,
						    v0 + static_cast<float>(Font::kCell) / Font::kHeight);
					}
					pen += f.advance[static_cast<size_t>(k)];
				}
			}
			Flat(out.blades, c, d, half, t, z1, true);
			Flat(out.blades, c, d, half, t, z0, false);
		}

		// a galvanised post, 6 cm square, from z0 to z1 at p - its collision too
		void Post(Meshes& out, roads::Collision& collision, const P2& p, float z0, float z1)
		{
			const float a = 0.03f;
			const std::array<P2, 4> q{P2{p[0] - a, p[1] - a}, P2{p[0] + a, p[1] - a}, P2{p[0] + a, p[1] + a}, P2{p[0] - a, p[1] + a}}; // (anticlockwise)
			for (size_t k = 0; k < 4; ++k)
			{
				const P2 &s0 = q[k], &s1 = q[(k + 1) % 4];
				const P2 r{(s1[0] - s0[0]) / (2 * a), (s1[1] - s0[1]) / (2 * a)}, nf{r[1], -r[0]}; // (outward: the right of going round)
				Quad(out.posts, s0, r, 2 * a, z0, z1, nf, 0, 2 * a, 0, z1 - z0);
				const uint32_t base = static_cast<uint32_t>(collision.vertices.size());
				collision.vertices.insert(collision.vertices.end(), {P3{s0[0], s0[1], z0}, P3{s1[0], s1[1], z0}, P3{s1[0], s1[1], z1}, P3{s0[0], s0[1], z1}});
				collision.triangles.push_back({base, base + 1, base + 2});
				collision.triangles.push_back({base, base + 2, base + 3});
				for (int i = 0; i < 2; ++i)
					collision.materials.push_back(kRailing), collision.kinds.push_back(kCollisionKind);
			}
		}

		// a code point of UTF-8 text read on from `i`
		uint32_t Next(const std::string& s, size_t& i)
		{
			const auto b = static_cast<unsigned char>(s[i++]);
			if (b < 0x80)
				return b;
			const int more = b >= 0xF0 ? 3 : b >= 0xE0 ? 2 : 1;
			uint32_t c = b & (0x3F >> more);
			for (int k = 0; k < more && i < s.size(); ++k)
				c = c << 6 | (static_cast<unsigned char>(s[i++]) & 0x3F);
			return c;
		}
	}

	texture::Image FontPicture()
	{
		return TheFont().picture;
	}

	std::string Abbreviate(const std::string& name)
	{
		// (ASCII: Latin-1's accented letters without their accents, others left out; the first of names joined by '/';
		// no full stops)
		static const char* const kLatin = "AAAAAAACEEEEIIIIDNOOOOOxOUUUUYPsaaaaaaaceeeeiiiidnooooo/ouuuuypy";
		std::string plain;
		for (size_t i = 0; i < name.size();)
		{
			const uint32_t c = Next(name, i);
			if (c == '/')
				break;
			if (c == '.')
				continue;
			if (c >= 32 && c < 127)
				plain += static_cast<char>(c);
			else if (c >= 0xC0 && c <= 0xFF)
				plain += kLatin[c - 0xC0];
		}
		std::vector<std::string> words;
		for (size_t i = 0; i < plain.size();)
		{
			const size_t j = plain.find(' ', i);
			if (const std::string w = plain.substr(i, j == std::string::npos ? std::string::npos : j - i); !w.empty())
				words.push_back(w);
			if (j == std::string::npos)
				break;
			i = j + 1;
		}
		if (words.empty())
			return {};
		static const std::map<std::string, std::string> kWays{{"North", "N"}, {"South", "S"}, {"East", "E"}, {"West", "W"}, {"Northeast", "NE"},
		    {"Northwest", "NW"}, {"Southeast", "SE"}, {"Southwest", "SW"}};
		static const std::map<std::string, std::string> kKinds{{"Street", "St"}, {"Avenue", "Ave"}, {"Boulevard", "Blvd"}, {"Road", "Rd"},
		    {"Drive", "Dr"}, {"Court", "Ct"}, {"Place", "Pl"}, {"Terrace", "Ter"}, {"Lane", "Ln"}, {"Circle", "Cir"}, {"Parkway", "Pkwy"},
		    {"Highway", "Hwy"}, {"Causeway", "Cswy"}, {"Trail", "Trl"}, {"Expressway", "Expy"}, {"Square", "Sq"}, {"Plaza", "Plz"}, {"Point", "Pt"},
		    {"Crescent", "Cres"}, {"Alley", "Aly"}, {"Heights", "Hts"}, {"Center", "Ctr"}, {"Freeway", "Fwy"}, {"Turnpike", "Tpke"}, {"Service", "Svc"},
		    {"Cove", "Cv"}, {"Ridge", "Rdg"}, {"Mount", "Mt"}, {"Fort", "Ft"}, {"Saint", "St"}};
		const size_t last = words.size() - 1;
		for (size_t k = 0; k < words.size(); ++k)
		{
			// (a direction at either end; the kinds anywhere but at the front - "Court Street", "Circle Drive")
			if ((k == 0 || k == last) && words.size() > 1)
				if (const auto it = kWays.find(words[k]); it != kWays.end())
				{
					words[k] = it->second;
					continue;
				}
			if (const auto it = kKinds.find(words[k]); it != kKinds.end() && (k > 0 || it->first == "Mount" || it->first == "Fort" || it->first == "Saint"))
				words[k] = it->second;
		}
		std::string out;
		for (const auto& w : words)
			out += (out.empty() ? "" : " ") + w;
		return out;
	}

	void Build(const roads::Network& n, const std::vector<props::Prop>& furniture, const std::function<bool(float, float)>& open,
	    const std::function<float(float, float)>& ground, const std::array<float, 4>& box, Meshes& out, roads::Collision& collision, Stats& stats)
	{
		const Font& font = TheFont();
		if (!font.cap)
			return;
		// the stop signs by 25 m cell
		const uint32_t stopSign = loader::convert::Joaat("prop_sign_road_01a");
		const auto key = [](float x, float y) {
			return static_cast<int64_t>(std::floor(x / 25)) << 32 | static_cast<uint32_t>(static_cast<int32_t>(std::floor(y / 25)));
		};
		std::unordered_map<int64_t, std::vector<const props::Prop*>> stops;
		for (const props::Prop& p : furniture)
			if (p.archetype == stopSign)
				stops[key(p.position[0], p.position[1])].push_back(&p);
		for (size_t vi = 0; vi < n.vertices.size(); ++vi)
		{
			const roads::Vertex& v = n.vertices[vi];
			if (v.edges.size() < 3 || v.p[0] < box[0] || v.p[1] < box[1] || v.p[0] >= box[2] || v.p[1] >= box[3])
				continue;
			struct Way
			{
				const roads::Edge* e;
				P2 in; // (the way traffic comes in, towards the junction)
				int rank;
				std::string name;
			};
			std::vector<Way> ways;
			for (const int ei : v.edges)
			{
				const roads::Edge& e = n.edges[static_cast<size_t>(ei)];
				const int rank = Rank(e);
				if (rank < 0 || e.points.size() < 2 || e.a == e.b || e.name.empty())
					continue;
				const bool atA = e.a == static_cast<int>(vi);
				const P2 p = atA ? e.points[0] : e.points.back(), q = atA ? e.points[1] : e.points[e.points.size() - 2];
				const float l = std::hypot(p[0] - q[0], p[1] - q[1]);
				if (l < 1e-3f)
					continue;
				if (std::string name = Abbreviate(e.name); !name.empty())
					ways.push_back({&e, {(p[0] - q[0]) / l, (p[1] - q[1]) / l}, rank, std::move(name)});
			}
			if (ways.empty())
				continue;
			// (the least street's name - its drivers stop -, and the biggest other)
			std::stable_sort(ways.begin(), ways.end(), [](const Way& a, const Way& b) { return a.rank < b.rank; });
			const Way& a = ways.front();
			const Way* b = nullptr;
			for (auto it = ways.rbegin(); it != ways.rend() && !b; ++it)
				if (it->name != a.name)
					b = &*it;
			if (!b || a.e->bridge || b->e->bridge)
				continue;
			// the mount: the stop sign nearest the junction within its corners' reach and 10 m more (on top of it), else a
			// post of their own on the right of the least street coming in, 1.3 m off its asphalt (a stop sign's 0.8:
			// further out), the left if that is not open ground
			const float reach = roads::CornerReach(n, static_cast<int>(vi));
			const props::Prop* stop = nullptr;
			float nearest = reach + 10;
			for (int dj = -1; dj <= 1; ++dj)
				for (int di = -1; di <= 1; ++di)
					if (const auto it = stops.find(key(v.p[0] + di * 25.0f, v.p[1] + dj * 25.0f)); it != stops.end())
						for (const props::Prop* p : it->second)
							if (const float dist = std::hypot(p->position[0] - v.p[0], p->position[1] - v.p[1]); dist < nearest)
								nearest = dist, stop = p;
			P2 at{};
			float top = 0, from = 0;
			if (stop)
			{
				at = {stop->position[0], stop->position[1]};
				from = stop->position[2] + 2.6f, top = stop->position[2] + 2.81f + 0.5f;
				++stats.onStops;
			}
			else
			{
				const P2 right{a.in[1], -a.in[0]};
				const float back = reach + 1.5f, aside = a.e->HalfWidth() + 1.3f;
				bool found = false;
				for (const float side : {1.0f, -1.0f})
				{
					const P2 q{v.p[0] - a.in[0] * back + right[0] * aside * side, v.p[1] - a.in[1] * back + right[1] * aside * side};
					if (open(q[0], q[1]))
					{
						at = q, found = true;
						break;
					}
				}
				if (!found)
					continue;
				from = ground(at[0], at[1]) - 0.1f, top = from + 0.1f + 3.3f;
				++stats.posts;
			}
			++stats.junctions;
			Post(out, collision, at, from, top + 0.03f);
			// (the cross street's blade over the least street's; 8 inch blades on the bigger streets)
			const float hbB = b->rank >= 3 ? 0.2032f : 0.1524f, hbA = a.rank >= 3 ? 0.2032f : 0.1524f;
			const float zB = top - 0.02f - hbB / 2, zA = zB - hbB / 2 - 0.02f - hbA / 2;
			Blade(out, font, b->name, at, zB, b->in, hbB);
			Blade(out, font, a.name, at, zA, a.in, hbA);
		}
	}
}
