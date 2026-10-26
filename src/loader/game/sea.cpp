#include "sea.hpp"

#include <Windows.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstring>
#include <fstream>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

#include "../config.hpp"
#include "../log.hpp"
#include "../paths.hpp"
#include "../pattern.hpp"
#include "natives.hpp"

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
		// The sea outside the grid drawn (0x36cc60): up to four strips round the grid, cut to the view, each a call of the
		// water quad drawing 0x36d3b0 (x0, x1, y0, y1, level, the four corners' alpha, a triangle's slope and offset: 0).
		constexpr uintptr_t kDrawQuad = 0x36D3B0;
		constexpr uintptr_t kStripCalls[] = {0x36CF9B, 0x36D011, 0x36D067, 0x36D0E2};
		constexpr uintptr_t kDefaultLevel = 0x2FAA758;
		// where the water is drawn in detail this frame (round the camera): min x, max x, min y, max y
		constexpr uintptr_t kDetailRegion = 0x2FAA770;

		struct Rect
		{
			int32_t x0, x1, y0, y1;
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
			// the water (water.bin): none when the pack has no mask
			bool water = false;
			float x0 = 0, y0 = 0, cell = 4;
			uint32_t nx = 0, ny = 0;
			std::vector<uint8_t> bits;
			Rect area{}; // the mask's box in whole metres
			int blocksX = 0, blocksY = 0, farX = 0, farY = 0;
			std::vector<std::vector<Rect>> nearRects, middleRects, farRects;

			bool Wet(int i, int j) const
			{
				const size_t k = static_cast<size_t>(j) * nx + static_cast<size_t>(i);
				return (bits[k >> 3] >> (k & 7)) & 1;
			}
		};

		std::vector<Land> g_lands;
		uintptr_t g_base = 0;
		bool g_patched = false;

		using DrawQuadFn = void(__fastcall*)(int32_t, int32_t, int32_t, int32_t, float, float, float, float, float, float, float);
		DrawQuadFn g_drawQuad = nullptr;

		// The cells [i0, i0 + n) x [j0, j0 + n) of the mask as rectangles of k x k cells (greedy: runs along x grown along y).
		std::vector<Rect> Cover(const Land& land, int i0, int j0, int n, int k)
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
							any = a < static_cast<int>(land.nx) && b < static_cast<int>(land.ny) && land.Wet(a, b);
						}
					coarse[static_cast<size_t>(j) * m + i] = any;
				}
			std::vector<Rect> out;
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
					out.push_back({edgeX(i0 + i * k), edgeX(i0 + i1 * k), edgeY(j0 + j * k), edgeY(j0 + j1 * k)});
					i = i1;
				}
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
			if (!in || std::memcmp(magic, "WBWM", 4) != 0 || version != 1 || land.cell <= 0 || !land.nx || !land.ny || land.nx > 65536 || land.ny > 65536)
				return false;
			land.bits.resize((static_cast<size_t>(land.nx) * land.ny + 7) / 8);
			in.read(reinterpret_cast<char*>(land.bits.data()), static_cast<std::streamsize>(land.bits.size()));
			if (!in)
				return false;
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
			land.water = true;
			log::Info("sea: land {}: water mask {} x {} cells of {:.0f} m; {} / {} / {} rectangles near / further / far", land.name, land.nx, land.ny,
			    land.cell, nearCount, middleCount, farCount);
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
					log::Info("sea: land {} ({:.0f}, {:.0f})..({:.0f}, {:.0f}), swell {:.2f} of the open ocean's", land.name, land.box[0], land.box[1],
					    land.box[2], land.box[3], land.waves);
					if (j.contains("water") && !ReadWater(land, entry.path() / j["water"].get<std::string>()))
						log::Warn("sea: land {}: no usable water mask ({}); the sea stays under it", land.name, j["water"].get<std::string>());
					g_lands.push_back(std::move(land));
				}
				catch (const std::exception& e)
				{
					log::Warn("sea: {}: land.json: {}", name, e.what());
				}
			}
		}

		// Whether the sea outside the game's grid is at a point (x, y, z): a land's mask in its box, else the sea.
		bool __fastcall SeaAt(const float* p)
		{
			for (const Land& land : g_lands)
				if (land.water && p[0] >= land.x0 && p[1] >= land.y0)
				{
					const auto i = static_cast<uint32_t>((p[0] - land.x0) / land.cell), j = static_cast<uint32_t>((p[1] - land.y0) / land.cell);
					if (i < land.nx && j < land.ny)
						return land.Wet(static_cast<int>(i), static_cast<int>(j));
				}
			return true;
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
					const Rect c{std::max(r.x0, clip.x0), std::min(r.x1, clip.x1), std::max(r.y0, clip.y0), std::min(r.y1, clip.y1)};
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
				if (!land.water)
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
			    [&](const Rect& r) { g_drawQuad(r.x0, r.x1, r.y0, r.y1, z, a1, a2, a3, a4, 0, 0); });
		}

		// The water's flat pass (0x3731d0, positions only: x, y as int16, z) writes the strips into 24 vertices (0x372f80:
		// up to four rectangles of two triangles, the rest zero). In its place: the game's strips (worked out into a buffer
		// of our own) split as above, into room for kFlatVertices.
		struct FlatVertex
		{
			int16_t x, y;
			float z;
		};
		constexpr int kFlatVertices = 0x600;
		constexpr uintptr_t kFlatBegin = 0x37391F, kFlatStrips = 0x37394E, kWriteStrips = 0x372F80;
		constexpr uint8_t kFlatBeginBytes[] = {0xBA, 0x18, 0x00, 0x00, 0x00}; // mov edx,18h: the vertices asked for
		using WriteStripsFn = void(__fastcall*)(FlatVertex*, int32_t, int32_t, int32_t, int32_t, float);
		WriteStripsFn g_writeStrips = nullptr;
		bool g_flatFull = false;

		void __fastcall FlatStrips(FlatVertex* out, int32_t minX, int32_t maxX, int32_t minY, int32_t maxY, float z)
		{
			FlatVertex strips[24]{};
			g_writeStrips(strips, minX, maxX, minY, maxY, z);
			int n = 0;
			const auto put = [&](const Rect& r) {
				if (n + 6 > kFlatVertices)
				{
					if (!g_flatFull)
						log::Warn("sea: the water's flat pass has room for {} rectangles; the rest of the water is left out of it", kFlatVertices / 6);
					g_flatFull = true;
					return;
				}
				const auto x0 = static_cast<int16_t>(r.x0), x1 = static_cast<int16_t>(r.x1), y0 = static_cast<int16_t>(r.y0), y1 = static_cast<int16_t>(r.y1);
				const FlatVertex v[6] = {{x0, y0, z}, {x1, y0, z}, {x1, y1, z}, {x0, y0, z}, {x1, y1, z}, {x0, y1, z}};
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
			bool ok = same(kLevelQuery, kLevelQueryBytes, sizeof(kLevelQueryBytes)) && same(kSegmentHit, kSegmentHitBytes, sizeof(kSegmentHitBytes));
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

			// the level query: SeaAt(r12) or no water
			Code level;
			level.Add({0x48, 0x83, 0xEC, 0x20});       // sub rsp,20h
			level.Add({0x4C, 0x89, 0xE1});             // mov rcx,r12
			level.MovRax(reinterpret_cast<uint64_t>(&SeaAt));
			level.Add({0xFF, 0xD0});                   // call rax
			level.Add({0x48, 0x83, 0xC4, 0x20});       // add rsp,20h
			level.Add({0x84, 0xC0, 0x74, 10 + 4 + 14}); // test al,al; jz (past the next three)
			level.MovRax(g_base + kDefaultLevel);        // (10 bytes)
			level.Add({0xF3, 0x0F, 0x10, 0x00});         // movss xmm0,[rax] (4)
			level.JumpTo(g_base + kLevelQueryWater);     // (14)
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
			const uintptr_t levelStub = place(level), segmentStub = place(segment), stripStub = place(strip), flatStub = place(flat);
			FlushInstructionCache(GetCurrentProcess(), stubs, 512);
			const auto callTo = [&](uintptr_t call, uintptr_t to) {
				uint8_t b[5] = {0xE8};
				const int32_t rel = static_cast<int32_t>(static_cast<intptr_t>(to) - static_cast<intptr_t>(g_base + call + 5));
				std::memcpy(&b[1], &rel, 4);
				Write(g_base + call, b, sizeof(b));
			};
			JumpPatch(g_base + kLevelQuery, levelStub, sizeof(kLevelQueryBytes));
			JumpPatch(g_base + kSegmentHit, segmentStub, sizeof(kSegmentHitBytes));
			for (const uintptr_t call : kStripCalls)
				callTo(call, stripStub);
			const uint8_t flatBegin[] = {0xBA, static_cast<uint8_t>(kFlatVertices & 0xFF), static_cast<uint8_t>(kFlatVertices >> 8), 0x00, 0x00};
			Write(g_base + kFlatBegin, flatBegin, sizeof(flatBegin));
			callTo(kFlatStrips, flatStub);
			log::Info("sea: the game's sea leaves the new land's boxes to their water");
			return true;
		}

		bool g_ours = false; // the deep ocean's scaler is ours (set while near a land)
		std::chrono::steady_clock::time_point g_next{};
	}

	bool Install()
	{
		ReadLands();
		if (std::none_of(g_lands.begin(), g_lands.end(), [](const Land& l) { return l.water; }))
			return false;
		g_patched = Patch();
		if (!g_patched)
			for (Land& land : g_lands)
				land.water = false;
		return g_patched;
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
		for (const Land& land : g_lands)
		{
			const float dx = std::max({land.box[0] - camera.x, 0.0f, camera.x - land.box[2]});
			const float dy = std::max({land.box[1] - camera.y, 0.0f, camera.y - land.box[3]});
			const float t = std::clamp(std::hypot(dx, dy) / kReach, 0.0f, 1.0f);
			want = std::min(want, land.waves + (1 - land.waves) * t * t * (3 - 2 * t));
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
