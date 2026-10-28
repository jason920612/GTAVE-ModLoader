#include "sea.hpp"

#include <Windows.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstring>
#include <fstream>
#include <string>
#include <thread>
#include <vector>

#include <nlohmann/json.hpp>

#include "../config.hpp"
#include "../log.hpp"
#include "../paths.hpp"
#include "../pattern.hpp"
#include "natives.hpp"
#include "sectors.hpp"

namespace loader::game::sea
{
	namespace
	{
		constexpr uint64_t kGetFinalRenderedCamCoord = 0xA200EB1EE790F448;
		constexpr uint64_t kGetDeepOceanScaler = 0x2B2A2CC86778B619;
		constexpr uint64_t kSetDeepOceanScaler = 0xB96B00E976BE977F;
		constexpr uint64_t kResetDeepOceanScaler = 0x5E5E99285AE812DB;
		constexpr float kReach = 1000; // how far out from a land its sea eases back to the open ocean's (m)

		// The game's water outside its grid (research/phase0.md §61.20).
		// The level query (0x358820), outside the grid: movss xmm0,[default level] (then: the level to [r15], 1 to [r14],
		// true); no water: xor eax,eax and out. The point is at r12.
		constexpr uintptr_t kLevelQuery = 0x35890B, kLevelQueryWater = 0x358913, kLevelQueryNone = 0x358B28;
		constexpr uint8_t kLevelQueryBytes[] = {0xF3, 0x0F, 0x10, 0x05, 0x45, 0x1E, 0xC5, 0x02};
		// The segment test (0x359fc0), a 500 m block outside the grid where the segment crosses the sea level: the crossing
		// just stored at [r8], then xorps xmm1,xmm1; roundss xmm1,xmm0,9 (is it in this block); the next block at 0x35A270.
		constexpr uintptr_t kSegmentHit = 0x35A3C8, kSegmentHitBack = 0x35A3D1, kSegmentNext = 0x35A270;
		constexpr uint8_t kSegmentHitBytes[] = {0x0F, 0x57, 0xC9, 0x66, 0x0F, 0x3A, 0x0A, 0xC8, 0x09};
		// The sea outside the grid drawn: up to four strips round the grid, cut to the view (0x36cc60: the far, flat water)
		// and the parts of the detailed region round the camera outside the grid (0x36f990, west, east, south, north, in its
		// two passes: the water with waves), each a call of the water quad drawing 0x36d3b0 (x0, x1, y0, y1, level, the
		// four corners' alpha, a triangle's slope and offset: 0).
		constexpr uintptr_t kDrawQuad = 0x36D3B0;
		constexpr uintptr_t kStripCalls[] = {0x36CF9B, 0x36D011, 0x36D067, 0x36D0E2, 0x3704FB, 0x370684, 0x3705D5, 0x370795, 0x370842, 0x370946, 0x3708D7,
		    0x370A13};
		// The waves' heights added to a level found (0x358b50: x, y, the level's address, a flag; the level query's two
		// callers 0x3587b0, 0x35ac40, which use nothing it returns): push r15; push r14; push r13 first.
		constexpr uintptr_t kWaves = 0x358B50;
		constexpr uint8_t kWavesBytes[] = {0x41, 0x57, 0x41, 0x56, 0x41, 0x55};
		constexpr uintptr_t kDefaultLevel = 0x2FAA758;
		// where the water is drawn in detail this frame (round the camera): min x, max x, min y, max y
		constexpr uintptr_t kDetailRegion = 0x2FAA770;

		struct Rect
		{
			int32_t x0, x1, y0, y1;
			float z = 0; // (a land's water: its level over the sea's)
		};

		// The rectangles drawn for a land's water, from its mask: blocks of 64 cells (256 m) at 8 m near the camera and 32 m
		// further, blocks of 256 cells (1 km) at 128 m far off. A coarse cell is water if any of its 4 m cells is: the land
		// it reaches over lies above the sea, so the water drawn under it does not show.
		constexpr int kBlock = 64, kFarBlock = 256;
		constexpr int kNear = 2, kMiddle = 8, kFar = 32; // cells a side of a coarse cell
		constexpr float kNearDistance = 500, kFarDistance = 2000;

		struct Land
		{
			std::string name;
			float box[4]{}; // min x, min y, max x, max y
			float waves = 1;
			std::string climate; // its weather (Climate below; empty: the game's)
			bool there = true;   // (false: a sector that waits, game/sectors.hpp - refreshed each Tick)
			// the water (water.bin): none when the pack has no mask; read when the land is first there (a sector that waits
			// may never be)
			std::filesystem::path waterFile;
			bool read = false;
			bool water = false;
			float x0 = 0, y0 = 0, cell = 4;
			uint32_t nx = 0, ny = 0;
			std::vector<uint8_t> bits;
			Rect area{}; // the mask's box in whole metres
			int blocksX = 0, blocksY = 0, farX = 0, farY = 0;
			std::vector<std::vector<Rect>> nearRects, middleRects, farRects;

			// (version 2: each water body at its own level - a lake, a river up the land - over the sea's: the levels, and
			// which of them each 8 x 8 cells' water is at)
			std::vector<float> levels{0.0f};
			std::vector<uint8_t> levelGrid;
			uint32_t lnx = 0, lny = 0;

			bool Wet(int i, int j) const
			{
				const size_t k = static_cast<size_t>(j) * nx + static_cast<size_t>(i);
				return (bits[k >> 3] >> (k & 7)) & 1;
			}
			uint8_t LevelIndex(int i, int j) const
			{
				return levelGrid.empty() ? 0 : levelGrid[static_cast<size_t>(j / 8) * lnx + static_cast<size_t>(i / 8)];
			}
		};

		std::vector<Land> g_lands;
		uintptr_t g_base = 0;
		bool g_patched = false;

		using DrawQuadFn = void(__fastcall*)(int32_t, int32_t, int32_t, int32_t, float, float, float, float, float, float, float);
		DrawQuadFn g_drawQuad = nullptr;

		// The cells [i0, i0 + n) x [j0, j0 + n) of the mask as rectangles of k x k cells (greedy: runs along x grown along y),
		// each level's on its own.
		void CoverLevel(const Land& land, int i0, int j0, int n, int k, uint8_t level, std::vector<Rect>& out)
		{
			const int m = (n + k - 1) / k;
			std::vector<uint8_t> coarse(static_cast<size_t>(m) * m, 0);
			for (int j = 0; j < m; ++j)
				for (int i = 0; i < m; ++i)
				{
					bool any = false;
					for (int dj = 0; dj < k && !any; ++dj)
						for (int di = 0; di < k && !any; ++di)
						{
							const int a = i0 + i * k + di, b = j0 + j * k + dj;
							any = a < static_cast<int>(land.nx) && b < static_cast<int>(land.ny) && land.Wet(a, b) && land.LevelIndex(a, b) == level;
						}
					coarse[static_cast<size_t>(j) * m + i] = any;
				}
			const float z = land.levels[level];
			const auto edgeX = [&](int cells) { return static_cast<int32_t>(std::lround(land.x0 + std::min<int>(cells, land.nx) * land.cell)); };
			const auto edgeY = [&](int cells) { return static_cast<int32_t>(std::lround(land.y0 + std::min<int>(cells, land.ny) * land.cell)); };
			for (int j = 0; j < m; ++j)
				for (int i = 0; i < m;)
				{
					if (!coarse[static_cast<size_t>(j) * m + i])
					{
						++i;
						continue;
					}
					int i1 = i;
					while (i1 < m && coarse[static_cast<size_t>(j) * m + i1])
						++i1;
					int j1 = j + 1;
					while (j1 < m && std::all_of(coarse.begin() + static_cast<size_t>(j1) * m + i, coarse.begin() + static_cast<size_t>(j1) * m + i1, [](uint8_t c) { return c != 0; }))
						++j1;
					for (int jj = j; jj < j1; ++jj)
						std::fill(coarse.begin() + static_cast<size_t>(jj) * m + i, coarse.begin() + static_cast<size_t>(jj) * m + i1, uint8_t{0});
					out.push_back({edgeX(i0 + i * k), edgeX(i0 + i1 * k), edgeY(j0 + j * k), edgeY(j0 + j1 * k), z});
					i = i1;
				}
		}

		std::vector<Rect> Cover(const Land& land, int i0, int j0, int n, int k)
		{
			std::vector<Rect> out;
			bool levels[256]{};
			for (int j = j0; j < std::min<int>(j0 + n, land.ny); j += 8)
				for (int i = i0; i < std::min<int>(i0 + n, land.nx); i += 8)
					levels[land.LevelIndex(i, j)] = true;
			for (int level = 0; level < 256; ++level)
				if (levels[level])
					CoverLevel(land, i0, j0, n, k, static_cast<uint8_t>(level), out);
			return out;
		}

		bool ReadWater(Land& land, const std::filesystem::path& file)
		{
			std::ifstream in(file, std::ios::binary);
			char magic[4]{};
			uint32_t version = 0;
			float level = 0;
			in.read(magic, 4);
			in.read(reinterpret_cast<char*>(&version), 4);
			in.read(reinterpret_cast<char*>(&land.x0), 4);
			in.read(reinterpret_cast<char*>(&land.y0), 4);
			in.read(reinterpret_cast<char*>(&land.cell), 4);
			in.read(reinterpret_cast<char*>(&land.nx), 4);
			in.read(reinterpret_cast<char*>(&land.ny), 4);
			in.read(reinterpret_cast<char*>(&level), 4);
			if (!in || std::memcmp(magic, "WBWM", 4) != 0 || (version != 1 && version != 2) || land.cell <= 0 || !land.nx || !land.ny || land.nx > 65536 ||
			    land.ny > 65536)
				return false;
			land.bits.resize((static_cast<size_t>(land.nx) * land.ny + 7) / 8);
			in.read(reinterpret_cast<char*>(land.bits.data()), static_cast<std::streamsize>(land.bits.size()));
			if (!in)
				return false;
			if (version == 2)
			{
				uint8_t n = 0;
				in.read(reinterpret_cast<char*>(&n), 1);
				if (!in || n == 0)
					return false;
				land.levels.resize(n);
				in.read(reinterpret_cast<char*>(land.levels.data()), static_cast<std::streamsize>(n * sizeof(float)));
				land.lnx = (land.nx + 7) / 8, land.lny = (land.ny + 7) / 8;
				land.levelGrid.resize(static_cast<size_t>(land.lnx) * land.lny);
				in.read(reinterpret_cast<char*>(land.levelGrid.data()), static_cast<std::streamsize>(land.levelGrid.size()));
				if (!in || std::any_of(land.levelGrid.begin(), land.levelGrid.end(), [&](uint8_t v) { return v >= n; }))
					return false;
			}
			land.area = {static_cast<int32_t>(std::lround(land.x0)), static_cast<int32_t>(std::lround(land.x0 + land.nx * land.cell)),
			    static_cast<int32_t>(std::lround(land.y0)), static_cast<int32_t>(std::lround(land.y0 + land.ny * land.cell))};
			land.blocksX = static_cast<int>((land.nx + kBlock - 1) / kBlock), land.blocksY = static_cast<int>((land.ny + kBlock - 1) / kBlock);
			land.farX = static_cast<int>((land.nx + kFarBlock - 1) / kFarBlock), land.farY = static_cast<int>((land.ny + kFarBlock - 1) / kFarBlock);
			size_t nearCount = 0, middleCount = 0, farCount = 0;
			for (int bj = 0; bj < land.blocksY; ++bj)
				for (int bi = 0; bi < land.blocksX; ++bi)
				{
					land.nearRects.push_back(Cover(land, bi * kBlock, bj * kBlock, kBlock, kNear));
					land.middleRects.push_back(Cover(land, bi * kBlock, bj * kBlock, kBlock, kMiddle));
					nearCount += land.nearRects.back().size(), middleCount += land.middleRects.back().size();
				}
			for (int fj = 0; fj < land.farY; ++fj)
				for (int fi = 0; fi < land.farX; ++fi)
				{
					land.farRects.push_back(Cover(land, fi * kFarBlock, fj * kFarBlock, kFarBlock, kFar));
					farCount += land.farRects.back().size();
				}
			// (set last: the drawing threads and the level queries look at it first - read on a thread of its own too)
			std::atomic_thread_fence(std::memory_order_release);
			land.water = true;
			log::Info("sea: land {}: water mask {} x {} cells of {:.0f} m, {} level(s); {} / {} / {} rectangles near / further / far", land.name, land.nx,
			    land.ny, land.cell, land.levels.size(), nearCount, middleCount, farCount);
			return true;
		}

		void ReadLands()
		{
			const auto& disabled = config::Get().disabledAssets;
			std::error_code ec;
			for (const auto& entry : std::filesystem::directory_iterator(paths::Get().mods, ec))
			{
				if (!entry.is_directory())
					continue;
				const std::string name = entry.path().filename().string();
				std::ifstream in(entry.path() / L"land.json");
				if (!in || disabled.contains(name))
					continue;
				try
				{
					const auto j = nlohmann::json::parse(in);
					Land land{name};
					for (int k = 0; k < 4; ++k)
						land.box[k] = j.at("box").at(k).get<float>();
					land.waves = std::clamp(j.value("waves", 1.0f), 0.0f, 1.0f);
					land.climate = j.value("climate", "");
					land.there = !sectors::Dormant(name);
					log::Info("sea: land {} ({:.0f}, {:.0f})..({:.0f}, {:.0f}), swell {:.2f} of the open ocean's", land.name, land.box[0], land.box[1],
					    land.box[2], land.box[3], land.waves);
					if (j.contains("water"))
						land.waterFile = entry.path() / j["water"].get<std::string>();
					g_lands.push_back(std::move(land));
				}
				catch (const std::exception& e)
				{
					log::Warn("sea: {}: land.json: {}", name, e.what());
				}
			}
		}

		// The water of the lands there not read yet (its rectangles worked out; until then - and if it is unusable - the land is
		// left out where the water is drawn: `water` set last, the drawing threads look at it first).
		void ReadThere()
		{
			for (Land& land : g_lands)
				if (land.there && !land.read && !land.waterFile.empty())
				{
					land.read = true;
					if (!ReadWater(land, land.waterFile))
						log::Warn("sea: land {}: no usable water mask ({}); the sea stays under it", land.name, land.waterFile.filename().string());
				}
		}

		// Whether there is water outside the game's grid at a point (x, y, z), and its level over the sea's: a land's mask in
		// its box (each water body at its level), else the sea.
		bool __fastcall LevelAt(const float* p, float* level)
		{
			for (const Land& land : g_lands)
				if (land.water && land.there && p[0] >= land.x0 && p[1] >= land.y0)
				{
					const auto i = static_cast<uint32_t>((p[0] - land.x0) / land.cell), j = static_cast<uint32_t>((p[1] - land.y0) / land.cell);
					if (i < land.nx && j < land.ny)
					{
						if (!land.Wet(static_cast<int>(i), static_cast<int>(j)))
							return false;
						*level = land.levels[land.LevelIndex(static_cast<int>(i), static_cast<int>(j))];
						return true;
					}
				}
			*level = 0;
			return true;
		}

		// The level query's answer: the sea's level and the water's over it (the game's [default level] + LevelAt's).
		bool __fastcall WaterAt(const float* p, float* level)
		{
			float over = 0;
			if (!LevelAt(p, &over))
				return false;
			*level = *reinterpret_cast<const float*>(g_base + kDefaultLevel) + over;
			return true;
		}

		// The waves' (0x358b50): none on the water up the land - a lake, a river: its surface is its meshes', flat; the sea's
		// waves (half a metre and more there) had boats and swimmers bob on a pond.
		bool __fastcall Calm(float x, float y)
		{
			const float p[3] = {x, y, 0};
			float over = 0;
			return LevelAt(p, &over) && over != 0;
		}

		// The segment test's: the sea's own water there (the game found its crossing of the sea level; water up the land
		// is not crossed there).
		bool __fastcall SeaAt(const float* p)
		{
			float over = 0;
			return LevelAt(p, &over) && over == 0;
		}

		float Distance(float x, float y, const Rect& r)
		{
			const float dx = std::max({static_cast<float>(r.x0) - x, 0.0f, x - static_cast<float>(r.x1)});
			const float dy = std::max({static_cast<float>(r.y0) - y, 0.0f, y - static_cast<float>(r.y1)});
			return std::sqrt(dx * dx + dy * dy);
		}

		// A land's water inside `clip` (within its box), each rectangle cut to it and handed to `emit`.
		template <typename Emit>
		void ForWater(const Land& land, const Rect& clip, Emit&& emit)
		{
			const auto* region = reinterpret_cast<const int32_t*>(g_base + kDetailRegion);
			const float cx = 0.5f * static_cast<float>(region[0] + region[1]), cy = 0.5f * static_cast<float>(region[2] + region[3]);
			const auto draw = [&](const std::vector<Rect>& rects) {
				for (const Rect& r : rects)
				{
					// (the water up the land - a lake's, a river's - is the land's own meshes': the game's renderer near the
					// camera draws all its water at the sea's level)
					if (r.z != 0)
						continue;
					const Rect c{std::max(r.x0, clip.x0), std::min(r.x1, clip.x1), std::max(r.y0, clip.y0), std::min(r.y1, clip.y1), r.z};
					if (c.x0 < c.x1 && c.y0 < c.y1)
						emit(c);
				}
			};
			const float side = land.cell * kBlock, farSide = land.cell * kFarBlock;
			const int fi0 = std::max(0, static_cast<int>(std::floor((clip.x0 - land.x0) / farSide))),
			          fi1 = std::min(land.farX - 1, static_cast<int>(std::floor((clip.x1 - land.x0) / farSide)));
			const int fj0 = std::max(0, static_cast<int>(std::floor((clip.y0 - land.y0) / farSide))),
			          fj1 = std::min(land.farY - 1, static_cast<int>(std::floor((clip.y1 - land.y0) / farSide)));
			for (int fj = fj0; fj <= fj1; ++fj)
				for (int fi = fi0; fi <= fi1; ++fi)
				{
					const Rect big{static_cast<int32_t>(land.x0 + fi * farSide), static_cast<int32_t>(land.x0 + (fi + 1) * farSide),
					    static_cast<int32_t>(land.y0 + fj * farSide), static_cast<int32_t>(land.y0 + (fj + 1) * farSide)};
					if (Distance(cx, cy, big) > kFarDistance)
					{
						draw(land.farRects[static_cast<size_t>(fj) * land.farX + fi]);
						continue;
					}
					for (int bj = fj * (kFarBlock / kBlock); bj < std::min(land.blocksY, (fj + 1) * (kFarBlock / kBlock)); ++bj)
						for (int bi = fi * (kFarBlock / kBlock); bi < std::min(land.blocksX, (fi + 1) * (kFarBlock / kBlock)); ++bi)
						{
							const Rect block{static_cast<int32_t>(land.x0 + bi * side), static_cast<int32_t>(land.x0 + (bi + 1) * side),
							    static_cast<int32_t>(land.y0 + bj * side), static_cast<int32_t>(land.y0 + (bj + 1) * side)};
							if (block.x1 <= clip.x0 || block.x0 >= clip.x1 || block.y1 <= clip.y0 || block.y0 >= clip.y1)
								continue;
							const size_t k = static_cast<size_t>(bj) * land.blocksX + bi;
							draw(Distance(cx, cy, block) < kNearDistance ? land.nearRects[k] : land.middleRects[k]);
						}
				}
		}

		// A strip of the sea outside the grid: the pieces of it outside the lands' boxes to `outside`, the lands' water in it
		// to `water`.
		template <typename Outside, typename Water>
		void SplitStrip(const Rect& strip, Outside&& outside, Water&& water)
		{
			std::vector<Rect> pieces{strip};
			for (const Land& land : g_lands)
			{
				if (!land.water || !land.there)
					continue;
				const Rect& b = land.area;
				std::vector<Rect> rest;
				for (const Rect& r : pieces)
				{
					if (r.x1 <= b.x0 || r.x0 >= b.x1 || r.y1 <= b.y0 || r.y0 >= b.y1)
					{
						rest.push_back(r);
						continue;
					}
					const Rect inside{std::max(r.x0, b.x0), std::min(r.x1, b.x1), std::max(r.y0, b.y0), std::min(r.y1, b.y1)};
					ForWater(land, inside, water);
					// what is left of the strip: south and north of the box (its whole width), west and east (between)
					if (r.y0 < inside.y0)
						rest.push_back({r.x0, r.x1, r.y0, inside.y0});
					if (inside.y1 < r.y1)
						rest.push_back({r.x0, r.x1, inside.y1, r.y1});
					if (r.x0 < inside.x0)
						rest.push_back({r.x0, inside.x0, inside.y0, inside.y1});
					if (inside.x1 < r.x1)
						rest.push_back({inside.x1, r.x1, inside.y0, inside.y1});
				}
				pieces = std::move(rest);
			}
			for (const Rect& r : pieces)
				outside(r);
		}

		// In place of the drawing of a strip of the sea outside the grid (0x36cc60).
		void __fastcall OuterStrip(int32_t x0, int32_t x1, int32_t y0, int32_t y1, float z, float a1, float a2, float a3, float a4, float slope, float offset)
		{
			SplitStrip(
			    {x0, x1, y0, y1}, [&](const Rect& r) { g_drawQuad(r.x0, r.x1, r.y0, r.y1, z, a1, a2, a3, a4, slope, offset); },
			    [&](const Rect& r) { g_drawQuad(r.x0, r.x1, r.y0, r.y1, z + r.z, a1, a2, a3, a4, 0, 0); });
		}

		// The water's flat pass (0x3731d0, positions only: x, y as int16, z) writes the strips into 24 vertices (0x372f80:
		// up to four rectangles of two triangles, the rest zero). In its place: the game's strips (worked out into a buffer
		// of our own) split as above, into room for kFlatVertices.
		struct FlatVertex
		{
			int16_t x, y;
			float z;
		};
		constexpr int kFlatVertices = 0x3000; // (2048 rectangles: a whole city's shores near the camera need over 256)
		constexpr uintptr_t kFlatBegin = 0x37391F, kFlatStrips = 0x37394E, kWriteStrips = 0x372F80;
		constexpr uint8_t kFlatBeginBytes[] = {0xBA, 0x18, 0x00, 0x00, 0x00}; // mov edx,18h: the vertices asked for
		using WriteStripsFn = void(__fastcall*)(FlatVertex*, int32_t, int32_t, int32_t, int32_t, float);
		WriteStripsFn g_writeStrips = nullptr;
		int g_flatMost = 0; // (the most rectangles a frame wanted, for the log when they do not fit)

		void __fastcall FlatStrips(FlatVertex* out, int32_t minX, int32_t maxX, int32_t minY, int32_t maxY, float z)
		{
			FlatVertex strips[24]{};
			g_writeStrips(strips, minX, maxX, minY, maxY, z);
			int n = 0, wanted = 0;
			const auto put = [&](const Rect& r) {
				++wanted;
				if (n + 6 > kFlatVertices)
					return;
				const auto x0 = static_cast<int16_t>(r.x0), x1 = static_cast<int16_t>(r.x1), y0 = static_cast<int16_t>(r.y0), y1 = static_cast<int16_t>(r.y1);
				const float rz = z + r.z;
				const FlatVertex v[6] = {{x0, y0, rz}, {x1, y0, rz}, {x1, y1, rz}, {x0, y0, rz}, {x1, y1, rz}, {x0, y1, rz}};
				std::memcpy(out + n, v, sizeof(v));
				n += 6;
			};
			for (int k = 0; k < 24; k += 6)
			{
				const Rect r{strips[k].x, strips[k + 2].x, strips[k].y, strips[k + 2].y};
				if (r.x0 < r.x1 && r.y0 < r.y1)
					SplitStrip(r, put, put);
			}
			std::memset(out + n, 0, sizeof(FlatVertex) * static_cast<size_t>(kFlatVertices - n));
			if (wanted * 6 > kFlatVertices && wanted > g_flatMost)
			{
				g_flatMost = wanted;
				log::Warn("sea: the water's flat pass has room for {} rectangles, {} wanted; the rest of the water is left out of it", kFlatVertices / 6, wanted);
			}
		}

		// Executable memory within 2 GB of `target` (the patches reach it with rel32).
		uint8_t* AllocateNear(uintptr_t target, size_t size)
		{
			SYSTEM_INFO info{};
			GetSystemInfo(&info);
			const uintptr_t granularity = info.dwAllocationGranularity;
			const uintptr_t start = target & ~(granularity - 1);
			for (uintptr_t step = granularity; step < 0x7FF00000; step += granularity)
				for (const uintptr_t at : {start - step, start + step})
					if (void* p = VirtualAlloc(reinterpret_cast<void*>(at), size, MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE))
						return static_cast<uint8_t*>(p);
			return nullptr;
		}

		void Write(uintptr_t at, const uint8_t* bytes, size_t size)
		{
			DWORD old;
			VirtualProtect(reinterpret_cast<void*>(at), size, PAGE_EXECUTE_READWRITE, &old);
			std::memcpy(reinterpret_cast<void*>(at), bytes, size);
			VirtualProtect(reinterpret_cast<void*>(at), size, old, &old);
			FlushInstructionCache(GetCurrentProcess(), reinterpret_cast<void*>(at), size);
		}

		// Machine code, appended.
		struct Code
		{
			std::vector<uint8_t> bytes;
			void Add(std::initializer_list<uint8_t> b) { bytes.insert(bytes.end(), b); }
			void Add64(uint64_t v)
			{
				for (int k = 0; k < 8; ++k)
					bytes.push_back(static_cast<uint8_t>(v >> (8 * k)));
			}
			void MovRax(uint64_t v) { Add({0x48, 0xB8}), Add64(v); }
			void JumpTo(uintptr_t to) { Add({0xFF, 0x25, 0, 0, 0, 0}), Add64(to); } // jmp [rip+0]
		};

		// A jmp rel32 at `at` to `to`, the rest of `length` bytes nops.
		void JumpPatch(uintptr_t at, uintptr_t to, size_t length)
		{
			std::vector<uint8_t> b(length, 0x90);
			b[0] = 0xE9;
			const int32_t rel = static_cast<int32_t>(static_cast<intptr_t>(to) - static_cast<intptr_t>(at + 5));
			std::memcpy(&b[1], &rel, 4);
			Write(at, b.data(), b.size());
		}

		bool Patch()
		{
			const auto module = pattern::Module::Main();
			g_base = module.base;
			const auto same = [&](uintptr_t rva, const uint8_t* bytes, size_t size) { return std::memcmp(reinterpret_cast<const void*>(g_base + rva), bytes, size) == 0; };
			bool ok = same(kLevelQuery, kLevelQueryBytes, sizeof(kLevelQueryBytes)) && same(kSegmentHit, kSegmentHitBytes, sizeof(kSegmentHitBytes)) &&
			          same(kWaves, kWavesBytes, sizeof(kWavesBytes));
			const auto calls = [&](uintptr_t call, uintptr_t target) {
				const auto* p = reinterpret_cast<const uint8_t*>(g_base + call);
				int32_t rel = 0;
				std::memcpy(&rel, p + 1, 4);
				return p[0] == 0xE8 && call + 5 + rel == target;
			};
			for (const uintptr_t call : kStripCalls)
				ok = ok && calls(call, kDrawQuad);
			ok = ok && same(kFlatBegin, kFlatBeginBytes, sizeof(kFlatBeginBytes)) && calls(kFlatStrips, kWriteStrips);
			if (!ok)
			{
				log::Warn("sea: the water code differs from the build this was made for; the sea stays under new land");
				return false;
			}
			uint8_t* stubs = AllocateNear(g_base, 512);
			if (!stubs)
				return false;
			g_drawQuad = reinterpret_cast<DrawQuadFn>(g_base + kDrawQuad);
			g_writeStrips = reinterpret_cast<WriteStripsFn>(g_base + kWriteStrips);

			// the level query: WaterAt(r12, &level) - that level, or no water
			Code level;
			level.Add({0x48, 0x83, 0xEC, 0x30});             // sub rsp,30h (the stack stays 16-byte aligned for the call)
			level.Add({0x4C, 0x89, 0xE1});                   // mov rcx,r12
			level.Add({0x48, 0x8D, 0x54, 0x24, 0x20});       // lea rdx,[rsp+20h]
			level.MovRax(reinterpret_cast<uint64_t>(&WaterAt));
			level.Add({0xFF, 0xD0});                         // call rax
			level.Add({0xF3, 0x0F, 0x10, 0x44, 0x24, 0x20}); // movss xmm0,[rsp+20h]
			level.Add({0x48, 0x83, 0xC4, 0x30});             // add rsp,30h
			level.Add({0x84, 0xC0, 0x74, 14});               // test al,al; jz (past the next)
			level.JumpTo(g_base + kLevelQueryWater);         // (14)
			level.JumpTo(g_base + kLevelQueryNone);

			// the segment test: SeaAt([r8]) or on to the next block
			Code segment;
			segment.Add({0x41, 0x50, 0x41, 0x51, 0x41, 0x52, 0x41, 0x53, 0x52}); // push r8, r9, r10, r11, rdx
			segment.Add({0x48, 0x83, 0xEC, 0x38});                         // sub rsp,38h
			segment.Add({0xF3, 0x0F, 0x7F, 0x44, 0x24, 0x20});             // movdqu [rsp+20h],xmm0
			segment.Add({0x4C, 0x89, 0xC1});                               // mov rcx,r8
			segment.MovRax(reinterpret_cast<uint64_t>(&SeaAt));
			segment.Add({0xFF, 0xD0});                                     // call rax
			segment.Add({0xF3, 0x0F, 0x6F, 0x44, 0x24, 0x20});             // movdqu xmm0,[rsp+20h]
			segment.Add({0x48, 0x83, 0xC4, 0x38});                         // add rsp,38h
			segment.Add({0x5A, 0x41, 0x5B, 0x41, 0x5A, 0x41, 0x59, 0x41, 0x58}); // pop rdx, r11, r10, r9, r8
			segment.Add({0x84, 0xC0, 0x74, 9 + 14});                       // test al,al; jz (past the next two)
			segment.Add({0x0F, 0x57, 0xC9, 0x66, 0x0F, 0x3A, 0x0A, 0xC8, 0x09}); // xorps xmm1,xmm1; roundss xmm1,xmm0,9
			segment.JumpTo(g_base + kSegmentHitBack);
			segment.JumpTo(g_base + kSegmentNext);

			// the waves: Calm(xmm0, xmm1) - out with the level as it is, or on into the function (its arguments kept)
			Code waves;
			waves.Add({0x48, 0x83, 0xEC, 0x48});             // sub rsp,48h (16-byte aligned for the call)
			waves.Add({0x48, 0x89, 0x4C, 0x24, 0x20});       // mov [rsp+20h],rcx
			waves.Add({0x48, 0x89, 0x54, 0x24, 0x28});       // mov [rsp+28h],rdx
			waves.Add({0x4C, 0x89, 0x44, 0x24, 0x30});       // mov [rsp+30h],r8
			waves.Add({0x4C, 0x89, 0x4C, 0x24, 0x38});       // mov [rsp+38h],r9
			waves.Add({0xF3, 0x0F, 0x11, 0x44, 0x24, 0x40}); // movss [rsp+40h],xmm0
			waves.Add({0xF3, 0x0F, 0x11, 0x4C, 0x24, 0x44}); // movss [rsp+44h],xmm1
			waves.MovRax(reinterpret_cast<uint64_t>(&Calm));
			waves.Add({0xFF, 0xD0});                         // call rax
			waves.Add({0x48, 0x8B, 0x4C, 0x24, 0x20});       // mov rcx,[rsp+20h]
			waves.Add({0x48, 0x8B, 0x54, 0x24, 0x28});       // mov rdx,[rsp+28h]
			waves.Add({0x4C, 0x8B, 0x44, 0x24, 0x30});       // mov r8,[rsp+30h]
			waves.Add({0x4C, 0x8B, 0x4C, 0x24, 0x38});       // mov r9,[rsp+38h]
			waves.Add({0xF3, 0x0F, 0x10, 0x44, 0x24, 0x40}); // movss xmm0,[rsp+40h]
			waves.Add({0xF3, 0x0F, 0x10, 0x4C, 0x24, 0x44}); // movss xmm1,[rsp+44h]
			waves.Add({0x48, 0x83, 0xC4, 0x48});             // add rsp,48h
			waves.Add({0x84, 0xC0, 0x74, 0x01, 0xC3});       // test al,al; jz (past the ret); ret
			waves.Add({0x41, 0x57, 0x41, 0x56, 0x41, 0x55}); // push r15; push r14; push r13 (the function's own)
			waves.JumpTo(g_base + kWaves + sizeof(kWavesBytes));

			// the strips: jumps on to OuterStrip and FlatStrips, which take the calls' arguments as they are
			Code strip, flat;
			strip.JumpTo(reinterpret_cast<uintptr_t>(&OuterStrip));
			flat.JumpTo(reinterpret_cast<uintptr_t>(&FlatStrips));

			uint8_t* at = stubs;
			const auto place = [&](const Code& c) {
				uint8_t* start = at;
				std::memcpy(at, c.bytes.data(), c.bytes.size());
				at += (c.bytes.size() + 15) & ~size_t{15};
				return reinterpret_cast<uintptr_t>(start);
			};
			const uintptr_t levelStub = place(level), segmentStub = place(segment), stripStub = place(strip), flatStub = place(flat), wavesStub = place(waves);
			FlushInstructionCache(GetCurrentProcess(), stubs, 512);
			const auto callTo = [&](uintptr_t call, uintptr_t to) {
				uint8_t b[5] = {0xE8};
				const int32_t rel = static_cast<int32_t>(static_cast<intptr_t>(to) - static_cast<intptr_t>(g_base + call + 5));
				std::memcpy(&b[1], &rel, 4);
				Write(g_base + call, b, sizeof(b));
			};
			JumpPatch(g_base + kLevelQuery, levelStub, sizeof(kLevelQueryBytes));
			JumpPatch(g_base + kSegmentHit, segmentStub, sizeof(kSegmentHitBytes));
			JumpPatch(g_base + kWaves, wavesStub, sizeof(kWavesBytes));
			for (const uintptr_t call : kStripCalls)
				callTo(call, stripStub);
			const uint8_t flatBegin[] = {0xBA, static_cast<uint8_t>(kFlatVertices & 0xFF), static_cast<uint8_t>(kFlatVertices >> 8 & 0xFF), 0x00, 0x00};
			Write(g_base + kFlatBegin, flatBegin, sizeof(flatBegin));
			callTo(kFlatStrips, flatStub);
			log::Info("sea: the game's sea leaves the new land's boxes to their water");
			return true;
		}

		// A land's climate: the weather by the hour, a few kinds each with its chance (picked afresh every game hour, the
		// same for the same day and hour). Tropical (Miami): clear mornings, afternoon thunderstorms, clearing evenings; dry:
		// sun, now and then haze; temperate: clouds, rain, morning fog.
		struct Chance
		{
			const char* weather;
			float share;
		};
		struct Hours
		{
			int from, to; // [from, to)
			std::vector<Chance> chances;
		};
		const std::vector<Hours>* Climate(const std::string& name)
		{
			static const std::vector<Hours> tropical{
			    {0, 7, {{"CLEAR", 0.6f}, {"CLOUDS", 0.3f}, {"FOGGY", 0.1f}}},
			    {7, 12, {{"EXTRASUNNY", 0.5f}, {"CLEAR", 0.35f}, {"CLOUDS", 0.15f}}},
			    {12, 14, {{"CLOUDS", 0.5f}, {"CLEAR", 0.4f}, {"OVERCAST", 0.1f}}},
			    {14, 18, {{"THUNDER", 0.25f}, {"RAIN", 0.2f}, {"OVERCAST", 0.25f}, {"CLOUDS", 0.3f}}},
			    {18, 20, {{"CLEARING", 0.4f}, {"CLOUDS", 0.4f}, {"CLEAR", 0.2f}}},
			    {20, 24, {{"CLEAR", 0.7f}, {"CLOUDS", 0.3f}}}};
			static const std::vector<Hours> dry{
			    {0, 10, {{"CLEAR", 0.8f}, {"SMOG", 0.2f}}}, {10, 18, {{"EXTRASUNNY", 0.75f}, {"CLEAR", 0.15f}, {"SMOG", 0.1f}}}, {18, 24, {{"CLEAR", 0.9f}, {"CLOUDS", 0.1f}}}};
			static const std::vector<Hours> temperate{
			    {0, 9, {{"FOGGY", 0.25f}, {"CLOUDS", 0.35f}, {"OVERCAST", 0.2f}, {"CLEAR", 0.2f}}},
			    {9, 17, {{"CLOUDS", 0.35f}, {"CLEAR", 0.25f}, {"OVERCAST", 0.2f}, {"RAIN", 0.15f}, {"CLEARING", 0.05f}}},
			    {17, 24, {{"CLOUDS", 0.4f}, {"CLEAR", 0.3f}, {"OVERCAST", 0.2f}, {"RAIN", 0.1f}}}};
			return name == "tropical" ? &tropical : name == "dry" ? &dry : name == "temperate" ? &temperate : nullptr;
		}

		const char* WeatherAt(const std::vector<Hours>& climate, int day, int hour)
		{
			for (const Hours& h : climate)
				if (hour >= h.from && hour < h.to)
				{
					uint32_t r = static_cast<uint32_t>(day * 31 + hour) * 2654435761u;
					r ^= r >> 15, r *= 0x2c1b3c6du, r ^= r >> 12;
					float pick = static_cast<float>(r % 10000) / 10000;
					for (const Chance& c : h.chances)
						if ((pick -= c.share) < 0)
							return c.weather;
					return h.chances.back().weather;
				}
			return "CLEAR";
		}

		constexpr uint64_t kGetClockHours = 0x25223CA6B4D20B7F, kGetClockDayOfMonth = 0x3D10BC92A4DB1D35;
		constexpr uint64_t kSetWeatherTypeOvertimePersist = 0xFB5045B7C42B75BF, kClearWeatherTypePersist = 0xCCC39339BEF76CF5;
		constexpr uint64_t kIsGameplayCamRendering = 0x39B5D1B10383F0C8;
		const char* g_weather = nullptr; // what the climate set last (nullptr: the game's own)

		bool g_ours = false; // the deep ocean's scaler is ours (set while near a land)
		std::chrono::steady_clock::time_point g_next{};
	}

	bool Install()
	{
		ReadLands();
		if (std::none_of(g_lands.begin(), g_lands.end(), [](const Land& l) { return !l.waterFile.empty(); }))
			return false;
		g_patched = Patch();
		if (!g_patched)
			for (Land& land : g_lands)
				land.waterFile.clear();
		ReadThere();
		return g_patched;
	}

	void Prepare(const std::string& name)
	{
		for (Land& land : g_lands)
			if (land.name == name && !land.read && !land.waterFile.empty())
			{
				// (the lands are all read at the start: this one stays where it is)
				land.read = true;
				Land* l = &land;
				std::thread([l] {
					if (!ReadWater(*l, l->waterFile))
						log::Warn("sea: land {}: no usable water mask ({}); the sea stays under it", l->name, l->waterFile.filename().string());
				}).detach();
			}
	}

	void Refresh()
	{
		for (Land& land : g_lands)
			land.there = !sectors::Dormant(land.name);
		g_next = {};
	}

	void Tick()
	{
		if (g_lands.empty())
			return;
		const auto now = std::chrono::steady_clock::now();
		if (now < g_next)
			return;
		g_next = now + std::chrono::milliseconds(250);

		const auto camera = natives::Invoke<natives::ScrVector>(kGetFinalRenderedCamCoord);
		float want = 1;
		const Land* on = nullptr; // the land the camera is over
		for (Land& land : g_lands)
			land.there = !sectors::Dormant(land.name);
		ReadThere();
		for (const Land& land : g_lands)
		{
			if (!land.there)
				continue;
			const float dx = std::max({land.box[0] - camera.x, 0.0f, camera.x - land.box[2]});
			const float dy = std::max({land.box[1] - camera.y, 0.0f, camera.y - land.box[3]});
			const float t = std::clamp(std::hypot(dx, dy) / kReach, 0.0f, 1.0f);
			want = std::min(want, land.waves + (1 - land.waves) * t * t * (3 - 2 * t));
			if (dx == 0 && dy == 0)
				on = &land;
		}
		// the land's weather (not under a scripted camera: the editor's, a cutscene's, a test's keep theirs)
		const auto* climate = on ? Climate(on->climate) : nullptr;
		if (climate && natives::Invoke<int32_t>(kIsGameplayCamRendering))
		{
			const char* weather = WeatherAt(*climate, natives::Invoke<int32_t>(kGetClockDayOfMonth), natives::Invoke<int32_t>(kGetClockHours));
			if (weather != g_weather)
			{
				natives::Invoke<void>(kSetWeatherTypeOvertimePersist, weather, 45.0f);
				log::Info("sea: {}'s climate ({}): {}", on->name, on->climate, weather);
				g_weather = weather;
			}
		}
		else if (!climate && g_weather)
		{
			// (off the land: the game's weather again)
			natives::Invoke<void>(kClearWeatherTypePersist);
			g_weather = nullptr;
		}
		if (want >= 0.999f)
		{
			// (away from every land: the game's own again, once)
			if (g_ours)
				natives::Invoke<void>(kResetDeepOceanScaler);
			g_ours = false;
			return;
		}
		// (kept: a script that sets it while we are at a land is overruled)
		if (std::abs(natives::Invoke<float>(kGetDeepOceanScaler) - want) > 0.01f)
			natives::Invoke<void>(kSetDeepOceanScaler, want);
		g_ours = true;
	}
}
