#include "minimap.hpp"

#include <Windows.h>
#include <intrin.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstring>
#include <filesystem>
#include <format>
#include <fstream>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

#include "../config.hpp"
#include "../log.hpp"
#include "../paths.hpp"
#include "../pattern.hpp"
#include "sectors.hpp"

namespace loader::game::minimap
{
	namespace
	{
		// The radar's supertiles: the game's 8 x 9 array of 0x78-byte entries (+0x00 row, +0x04 column, +0x08 loaded,
		// +0x0C its streaming request: +0x0C the slot, -1 none, +0x10 flags, 0x7F00 none; +0x18 the foreground's nine
		// models, +0x60 the land's, +0x68 the water's; +0x70 load asked, +0x71 release asked, +0x72 wanted).
		constexpr uintptr_t kSupertiles = 0x3DF8240;
		constexpr size_t kEntry = 0x78, kGameEntries = 72;
		constexpr int kIndexBase = 1000; // a land's supertile's numbers: its place on the lattice + 1000
		// Which are wanted: 0x570250 (the array; the view's middle and its half size in the lattice's tiles, six a
		// supertile a side; "none"), called from 0x571150.
		constexpr uintptr_t kSelect = 0x570250, kSelectCall = 0x5711E5;
		// Each entry streamed (its model dictionary asked for when wanted, its models found when in, released when not):
		// 0x56FB80, from 0x570540 for them all, called from 0x569980.
		constexpr uintptr_t kStream = 0x56FB80, kStreamAll = 0x570540, kStreamAllCall = 0x569D09;
		// Drawn (render thread): 0x56ECA0 (an entry; 0: its water and land, 1: its foreground), from 0x570630 (all with 0,
		// then all with 1), called from 0x57BCD0.
		constexpr uintptr_t kDraw = 0x56ECA0, kDrawAll = 0x570630, kDrawAllCall = 0x57C1D9;
		// The session's end: 0x5700B0 (the array, the shutdown's kind: 8 releases them all), jumped to from 0x568F60.
		constexpr uintptr_t kShutdown = 0x5700B0, kShutdownJump = 0x56903F;
		// The streaming request's own: its flags cleared (two kinds), released; asked for (the request, the slot, its store's
		// index - the drawable dictionaries' at +0x3ED63B4 -, flags: 0x56FB80 asks with 7, 0x56FDA8).
		constexpr uintptr_t kClearA = 0xCF2420, kClearB = 0xCF24C0, kRelease = 0xCF2590, kAsk = 0xCF2200, kAskCall = 0x56FDA8,
		                    kDrawableStore = 0x3ED63B4;
		// A drawable dictionary's slot by its name, as the radar's stream finds one (0x56FC25): the game's string hash (0xAA4680,
		// kind 0) looked up in the store's map of names - +0x3ED6428 its buckets (a first node's index each), +0x3ED6430 how
		// many, +0x3ED6420 its nodes (12 bytes: the name's hash, the slot, the next node's index; -1 none).
		constexpr uintptr_t kHash = 0xAA4680, kNameBuckets = 0x3ED6428, kNameBucketCount = 0x3ED6430, kNameNodes = 0x3ED6420;
		// The radar's lattice (minimap.ymt; worldgen's minimap.cpp): a supertile's metres, six tiles a side.
		constexpr float kSuperW = 9400.0f / 8, kSuperH = 12492.0f / 9;
		// The thread's state the game checks first: its slot in thread-local storage (+0x590 & 6: not this thread's work).
		constexpr uintptr_t kTlsIndex = 0x2E97518;

		// The pause map's pictures (minimap.ymt, in memory: +0 columns, +2 rows (u16), +4 a square's size, +0xC the grid's
		// north-west corner, x and y): their array is made at the session's start from it, in 0x564310 (kind 8; the game's
		// init system calls it through a pointer: hooked at its start, whose first five bytes - push r14, rsi, rdi, rbp -
		// run from a trampoline); each square's dictionary named formatf("%s_%d_%d", layer, row, column) at 0x55E6B8 (the
		// layers minimap_sea, minimap and minimap_alpha).
		constexpr uintptr_t kPictureGrid = 0x3DEAB64, kPictureInit = 0x564310, kFormat = 0xD04C0, kFormatCall = 0x55E6B8;
		constexpr uint8_t kPictureInitStart[] = {0x41, 0x56, 0x56, 0x57, 0x55};
		// How far the pause map's cursor goes: 0x129C7F0 (?, the north-west corner (min x, max y), the south-east one (max
		// x, min y), ?) works it out (the map's area 300 m out, and where the player is), called from 0x129A760 (the map
		// moved by the controls: the cursor is held to it) twice and from 0x129E470.
		constexpr uintptr_t kReach = 0x129C7F0, kReachCalls[] = {0x129AB19, 0x129AB3C, 0x129E7DF};
		constexpr float kReachMargin = 300;
		// The pause map's squares (research/phase0.md §63.15; the radar draws their pictures too, zoomed out): +0x3DEB760 their
		// array (16 bytes a square: +0 its dictionary's slot in the texture store, -1 none; +4 its state - 0 none, 1 asked
		// for, 2-3 in use, 4-5 being let go: the render thread's last use waited for -; +8 the slot each of the render
		// thread's two buffers draws), +8 how many. 0x55E990 takes one a step on (the square, wanted, drawn, its name - looked
		// up in state 0 only), for each from 0x55E460: a square in use keeps its dictionary while wanted, whatever its name
		// has become.
		constexpr uintptr_t kSquares = 0x3DEB760, kSquareStep = 0x55E990;
		constexpr uint8_t kSquareStepStart[] = {0x41, 0x57, 0x41, 0x56, 0x56, 0x57, 0x53, 0x48, 0x83, 0xEC, 0x20, 0x44, 0x89, 0xC3, 0x48, 0x89, 0xCE, 0x8B, 0x49, 0x04};

		using SelectFn = bool (*)(uint8_t*, const float*, const float*, uint64_t, uint8_t);
		using EntryFn = void (*)(uint8_t*);
		using DrawFn = void (*)(uint8_t*, int);
		using ShutdownFn = void (*)(uint8_t*, int);
		using RequestFn = void (*)(void*, int);
		using ReleaseFn = void (*)(void*);
		using AskFn = uint8_t (*)(void*, int32_t, uint32_t, uint32_t);
		using InitFn = void (*)(int);
		using ReachFn = void (*)(void*, float*, float*, uint64_t);
		using SquareStepFn = void (*)(uint8_t* square, uint8_t wanted, uint8_t drawn, const char* name);

		uintptr_t g_base = 0;
		SelectFn g_select = nullptr;
		EntryFn g_stream = nullptr, g_streamAll = nullptr;
		DrawFn g_draw = nullptr;
		ShutdownFn g_shutdown = nullptr;
		RequestFn g_clearA = nullptr, g_clearB = nullptr;
		ReleaseFn g_release = nullptr;
		AskFn g_ask = nullptr;
		InitFn g_pictureInit = nullptr;
		ReachFn g_reach = nullptr;
		SquareStepFn g_squareStep = nullptr;
		std::vector<std::string> g_going; // (lands being put away: their pictures named no more)
		// each land: its pack's folder, its box (min x, min y, max x, max y)
		struct Land
		{
			std::string folder;
			std::array<float, 4> box{};
		};
		std::vector<Land> g_lands;

		// a land's supertile: its place on the lattice and its entry (ours, as the game's)
		struct Supertile
		{
			int column = 0, row = 0;
			uint8_t* entry = nullptr;
			std::vector<std::string> owners; // (the lands that have it - sectors sharing a place: only the one there draws)
		};
		std::vector<Supertile> g_supertiles;
		std::vector<uint8_t> g_entries;
		bool g_suspended = false; // (a sector switch under way: none wanted)

		// The lands registered for a crossing, their supertiles under names of their own (OwnName): each an entry like the
		// game's that asks for its file's slot itself - the game's way (0x56FB80) asks by the name its numbers make, the land
		// there's - its models then found by those numbers as the game's are (supertile_back_<column>_<row>...). Drawn while
		// the land is the player's; streamed too while it arrives, for where the player arrives.
		struct Own
		{
			struct Tile
			{
				int column = 0, row = 0;
				int32_t slot = -1;
				uint8_t entry[kEntry]{};
				bool asked = false;
				bool wanted = false; // (the selection's word - another thread's -, asked for by the stream on the game's own)
			};
			std::vector<std::unique_ptr<Tile>> tiles;
			float shift[2]{}; // (arriving: where the player arrives, in the lattice's tiles from the view's middle)
			bool arriving = false, drawn = false, going = false;
			int quiet = 0; // (frames since its tiles were all let go: the render thread's last use)
		};
		std::map<std::string, Own> g_own;
		std::mutex g_ownMutex; // (the render thread draws them)
		std::map<std::string, int> g_numbers; // (each land's number in its supertiles' names)

		// the pause map: the lands' squares (row, column on the lattice) -> the lands that have a picture of it (their pack's
		// folder, the pictures' name); the open sea's (the first land with pictures that is there)
		std::map<std::pair<int, int>, std::vector<std::pair<std::string, std::string>>> g_pictures;
		std::vector<std::pair<std::string, std::string>> g_seas;
		// the game's grid as minimap.ymt has it, ours, and the lattice's place of our grid's first square
		struct Grid
		{
			uint16_t columns = 0, rows = 0;
			float x = 0, y = 0;
		};
		Grid g_game, g_ours;
		float g_square = 0;
		int g_firstRow = 0, g_firstColumn = 0;
		bool g_gridKnown = false;

		void ResetEntry(uint8_t* e)
		{
			std::memset(e, 0, kEntry);
			*reinterpret_cast<int32_t*>(e + 0x0C) = -1;
			*reinterpret_cast<uint16_t*>(e + 0x10) = 0x7F00;
		}

		// The game's own check at the top of its loops over the entries (lazily set up by then: the game's own loop ran).
		bool NotThisThread()
		{
			const uint32_t index = *reinterpret_cast<const uint32_t*>(g_base + kTlsIndex);
			const auto* slots = reinterpret_cast<uint8_t* const*>(__readgsqword(0x58));
			const uint8_t* tls = slots ? slots[index] : nullptr;
			return !tls || tls[0x638] == 0 || (tls[0x590] & 6) != 0;
		}

		int FloorSixth(int v)
		{
			return v >= 0 ? v / 6 : -((-v + 5) / 6);
		}

		bool Select(uint8_t* array, const float* middle, const float* half, uint64_t r9, uint8_t none)
		{
			const bool result = g_select(array, middle, half, r9, none);
			// as the game does for its own (rows and columns of tiles, six a supertile), not held to its 8 x 9
			const int rowFrom = FloorSixth(static_cast<int>(std::floor(middle[1] - half[1]))), rowTo = FloorSixth(static_cast<int>(std::ceil(middle[1] + half[1])));
			const int columnFrom = FloorSixth(static_cast<int>(std::floor(middle[0] - half[0]))),
			          columnTo = FloorSixth(static_cast<int>(std::ceil(middle[0] + half[0])));
			std::lock_guard lock(g_ownMutex);
			for (Supertile& s : g_supertiles)
			{
				// (a land with supertiles of its own draws those, not the pack's names - the land registered first's files)
				const bool there = std::any_of(s.owners.begin(), s.owners.end(), [](const std::string& o) { return !sectors::Dormant(o) && !g_own.contains(o); });
				const uint8_t want = !none && !g_suspended && there && s.row >= rowFrom && s.row <= rowTo && s.column >= columnFrom && s.column <= columnTo;
				uint8_t* e = s.entry;
				if (e[0x72] != want)
				{
					e[want ? 0x70 : 0x71] = 1;
					e[0x72] = want;
				}
			}
			// (the selection runs on a thread of its own - 0x571150's, not the game's main one -: it only marks, as the game's
			// does; the asking is the stream's, on the main thread. Asked from here, the request met the main thread's own in
			// the streaming's request map, unlocked, and its free list was broken: crash 0xCE9491)
			for (auto& [land, own] : g_own)
			{
				const bool here = !own.going && !sectors::Dormant(land);
				own.drawn = here;
				if (here)
					own.arriving = false;
				const bool active = here || (!own.going && own.arriving);
				const float sx = here ? 0.0f : own.shift[0], sy = here ? 0.0f : own.shift[1];
				const int ownRowFrom = FloorSixth(static_cast<int>(std::floor(middle[1] + sy - half[1]))),
				          ownRowTo = FloorSixth(static_cast<int>(std::ceil(middle[1] + sy + half[1])));
				const int ownColumnFrom = FloorSixth(static_cast<int>(std::floor(middle[0] + sx - half[0]))),
				          ownColumnTo = FloorSixth(static_cast<int>(std::ceil(middle[0] + sx + half[0])));
				for (auto& t : own.tiles)
				{
					const bool want = active && !none && !g_suspended && t->slot >= 0 && t->row >= ownRowFrom && t->row <= ownRowTo &&
					                  t->column >= ownColumnFrom && t->column <= ownColumnTo;
					uint8_t* e = t->entry;
					t->wanted = want;
					if (!want && t->asked)
					{
						e[0x71] = 1;
						e[0x72] = 0;
						t->asked = false;
					}
				}
			}
			return result;
		}

		void StreamAll(uint8_t* array)
		{
			g_streamAll(array);
			if (g_supertiles.empty() || NotThisThread())
				return;
			for (Supertile& s : g_supertiles)
				g_stream(s.entry);
			const uint32_t store = *reinterpret_cast<const uint32_t*>(g_base + kDrawableStore);
			std::lock_guard lock(g_ownMutex);
			for (auto& [land, own] : g_own)
				for (auto& t : own.tiles)
				{
					if (t->wanted && !t->asked)
					{
						// (as the game's own stream asks when it found the slot by name: 0x56FDA8)
						g_ask(t->entry + 0x0C, t->slot, store, 7);
						t->entry[0x72] = 1;
						t->asked = true;
					}
					g_stream(t->entry);
				}
		}

		void DrawAll(uint8_t* array)
		{
			std::lock_guard lock(g_ownMutex);
			for (int pass = 0; pass < 2; ++pass)
			{
				for (size_t k = 0; k < kGameEntries; ++k)
					g_draw(array + k * kEntry, pass);
				for (Supertile& s : g_supertiles)
					g_draw(s.entry, pass);
				for (auto& [land, own] : g_own)
					if (own.drawn)
						for (auto& t : own.tiles)
							g_draw(t->entry, pass);
			}
		}

		// An entry's models and its dictionary let go, as the game's own release does (0x56FB80).
		void Release(uint8_t* e)
		{
			std::memset(e + 0x18, 0, 0x58);
			g_clearA(e + 0x0C, 1);
			g_clearB(e + 0x0C, 1);
			g_release(e + 0x0C);
			e[0x08] = 0;
			e[0x70] = e[0x71] = e[0x72] = 0;
		}

		void Shutdown(uint8_t* array, int kind)
		{
			g_shutdown(array, kind);
			if (kind != 8 || g_supertiles.empty() || NotThisThread())
				return;
			for (Supertile& s : g_supertiles)
				Release(s.entry);
			std::lock_guard lock(g_ownMutex);
			for (auto& [land, own] : g_own)
				for (auto& t : own.tiles)
					Release(t->entry);
			g_own.clear();
		}

		int32_t SlotOf(const std::string& name)
		{
			const uint32_t hash = reinterpret_cast<uint32_t (*)(int, const char*)>(g_base + kHash)(0, name.c_str());
			const auto* buckets = *reinterpret_cast<const uint32_t* const*>(g_base + kNameBuckets);
			const uint32_t count = *reinterpret_cast<const uint32_t*>(g_base + kNameBucketCount);
			const auto* nodes = *reinterpret_cast<const uint32_t* const*>(g_base + kNameNodes);
			if (!buckets || !count || !nodes)
				return -1;
			for (uint32_t i = buckets[hash % count]; i != 0xFFFFFFFF; i = nodes[static_cast<size_t>(i) * 3 + 2])
				if (nodes[static_cast<size_t>(i) * 3] == hash)
					return static_cast<int32_t>(nodes[static_cast<size_t>(i) * 3 + 1]);
			return -1;
		}

		// "minimap_<column + 1000>_<row + 1000>" in a file's name (any folder, any extension): the two numbers, and where the
		// first starts and the second ends
		bool TileNumbers(std::string_view name, int& c, int& r, size_t& from, size_t& to)
		{
			const size_t slash = name.find_last_of("/\\");
			const size_t start = slash == std::string_view::npos ? 0 : slash + 1;
			if (name.substr(start, 8) != "minimap_")
				return false;
			size_t i = start + 8;
			const auto number = [&](int& v) {
				const size_t first = i;
				v = 0;
				while (i < name.size() && name[i] >= '0' && name[i] <= '9' && i - first < 9)
					v = v * 10 + (name[i++] - '0');
				return i > first;
			};
			from = i;
			if (!number(c) || i >= name.size() || name[i++] != '_' || !number(r))
				return false;
			to = i;
			return i == name.size() || name[i] == '.';
		}

		// The pause map's grid widened to the lands' squares (each session start: minimap.ymt is read once, at the start).
		void PictureInit(int kind)
		{
			if (kind == 8)
			{
				auto* grid = reinterpret_cast<uint8_t*>(g_base + kPictureGrid);
				auto& columns = *reinterpret_cast<uint16_t*>(grid);
				auto& rows = *reinterpret_cast<uint16_t*>(grid + 2);
				auto* corner = reinterpret_cast<float*>(grid + 0xC);
				if (!g_gridKnown && columns && rows && *reinterpret_cast<const float*>(grid + 4) > 0)
				{
					g_game = {columns, rows, corner[0], corner[1]};
					g_square = *reinterpret_cast<const float*>(grid + 4);
					int r0 = 0, c0 = 0, r1 = g_game.rows - 1, c1 = g_game.columns - 1;
					for (const auto& [at, land] : g_pictures)
						r0 = std::min(r0, at.first), r1 = std::max(r1, at.first), c0 = std::min(c0, at.second), c1 = std::max(c1, at.second);
					g_firstRow = r0, g_firstColumn = c0;
					g_ours = {static_cast<uint16_t>(c1 - c0 + 1), static_cast<uint16_t>(r1 - r0 + 1), g_game.x + c0 * g_square, g_game.y - r0 * g_square};
					g_gridKnown = true;
					log::Info("minimap: the pause map's pictures {} x {} -> {} x {} squares of {:.0f} m from ({:.0f}, {:.0f})", g_game.columns, g_game.rows, g_ours.columns,
					    g_ours.rows, g_square, g_ours.x, g_ours.y);
				}
				if (g_gridKnown && columns == g_game.columns && rows == g_game.rows && corner[0] == g_game.x && corner[1] == g_game.y)
				{
					columns = g_ours.columns, rows = g_ours.rows;
					corner[0] = g_ours.x, corner[1] = g_ours.y;
				}
			}
			g_pictureInit(kind);
		}

		// The pause map's cursor reaches the lands too (300 m round each, as the game's own area).
		void Reach(void* a, float* northWest, float* southEast, uint64_t d)
		{
			g_reach(a, northWest, southEast, d);
			for (const Land& land : g_lands)
			{
				if (sectors::Dormant(land.folder))
					continue;
				const auto& box = land.box;
				northWest[0] = std::min(northWest[0], box[0] - kReachMargin);
				northWest[1] = std::max(northWest[1], box[3] + kReachMargin);
				southEast[0] = std::max(southEast[0], box[2] + kReachMargin);
				southEast[1] = std::min(southEast[1], box[1] - kReachMargin);
			}
		}

		// Whether a land is there (not a sector that waits, nor one being put away).
		bool Present(const std::string& folder)
		{
			return !sectors::Dormant(folder) && std::find(g_going.begin(), g_going.end(), folder) == g_going.end();
		}

		// The pictures' name of the land there with a picture of that square, or null.
		const std::string* There(const std::map<std::pair<int, int>, std::vector<std::pair<std::string, std::string>>>& pictures, std::pair<int, int> at)
		{
			const auto it = pictures.find(at);
			if (it == pictures.end())
				return nullptr;
			for (const auto& [folder, name] : it->second)
				if (Present(folder))
					return &name;
			return nullptr;
		}

		// A square's dictionary: the land's picture of it, the game's own, or the open sea's.
		char* PictureName(char* buffer, const char* format, const char* layer, int row, int column)
		{
			(void)format;
			const int r = row + g_firstRow, c = column + g_firstColumn;
			std::string name;
			if (!g_gridKnown)
				name = std::format("{}_{}_{}", layer, row, column); // (the grid not widened: the game's own names)
			else if (const auto* land = There(g_pictures, {r, c}))
				name = std::format("{}_{}_{}_{}", layer, *land, r, c);
			else if (r >= 0 && r < g_game.rows && c >= 0 && c < g_game.columns)
				name = std::format("{}_{}_{}", layer, r, c);
			else
			{
				// (no land there - a switch under way -: no picture, rather than one of a land that is going)
				const auto sea = std::find_if(g_seas.begin(), g_seas.end(), [](const auto& s) { return Present(s.first); });
				name = sea != g_seas.end() ? std::format("{}_{}_sea", layer, sea->second) : std::format("{}_none", layer);
			}
			const size_t n = std::min<size_t>(name.size(), 49); // (the game's buffer: 50)
			std::memcpy(buffer, name.data(), n);
			buffer[n] = 0;
			return buffer;
		}

		void ReadLands()
		{
			const auto& disabled = config::Get().disabledAssets;
			std::error_code ec;
			for (const auto& dir : paths::LandFolders())
			{
				const std::string folder = dir.filename().string();
				std::ifstream in(dir / L"land.json");
				if (!in || disabled.contains(folder))
					continue;
				try
				{
					const auto j = nlohmann::json::parse(in);
					if (!j.contains("map"))
						continue;
					const auto& map = j.at("map");
					const std::string name = map.value("name", folder);
					size_t radar = 0, pictures = 0;
					for (const auto& rc : map.value("radar", nlohmann::json::array()))
					{
						const int column = rc.at(0).get<int>(), row = rc.at(1).get<int>();
						// (a supertile other lands have too - sectors sharing a place - is one entry: the same file name, the
						// file of the land there)
						const auto same = std::find_if(g_supertiles.begin(), g_supertiles.end(), [&](const Supertile& s) { return s.column == column && s.row == row; });
						if (same != g_supertiles.end())
							same->owners.push_back(folder);
						else
							g_supertiles.push_back({column, row, nullptr, {folder}});
						++radar;
					}
					for (const auto& rc : map.value("pictures", nlohmann::json::array()))
					{
						g_pictures[std::make_pair(rc.at(0).get<int>(), rc.at(1).get<int>())].push_back({folder, name});
						++pictures;
					}
					if (pictures)
						g_seas.push_back({folder, name});
					if (j.contains("box"))
						g_lands.push_back({folder, {j.at("box").at(0).get<float>(), j.at("box").at(1).get<float>(), j.at("box").at(2).get<float>(), j.at("box").at(3).get<float>()}});
					log::Info("minimap: land {}: {} radar supertiles, {} pause map pictures", folder, radar, pictures);
				}
				catch (const std::exception& e)
				{
					log::Warn("minimap: {}\\land.json: {}", folder, e.what());
				}
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

		// The rel32 call (E8) or jump (E9) at `at`, if it goes to `target`.
		bool Goes(uintptr_t at, uint8_t opcode, uintptr_t target)
		{
			const auto* p = reinterpret_cast<const uint8_t*>(g_base + at);
			int32_t rel = 0;
			std::memcpy(&rel, p + 1, 4);
			return p[0] == opcode && at + 5 + rel == target;
		}
	}

	bool Install()
	{
		ReadLands();
		if (g_supertiles.empty() && g_pictures.empty())
			return false;
		g_base = pattern::Module::Main().base;
		if (!Goes(kSelectCall, 0xE8, kSelect) || !Goes(kStreamAllCall, 0xE8, kStreamAll) || !Goes(kDrawAllCall, 0xE8, kDrawAll) ||
		    !Goes(kShutdownJump, 0xE9, kShutdown) || !Goes(kFormatCall, 0xE8, kFormat) ||
		    std::any_of(std::begin(kReachCalls), std::end(kReachCalls), [](uintptr_t call) { return !Goes(call, 0xE8, kReach); }) ||
		    std::memcmp(reinterpret_cast<const void*>(g_base + kPictureInit), kPictureInitStart, sizeof(kPictureInitStart)) != 0)
		{
			log::Warn("minimap: the map code differs from the build this was made for; the new land is not on the game's map");
			return false;
		}
		uint8_t* stubs = AllocateNear(g_base, 256);
		if (!stubs)
			return false;
		g_select = reinterpret_cast<SelectFn>(g_base + kSelect);
		g_stream = reinterpret_cast<EntryFn>(g_base + kStream);
		g_streamAll = reinterpret_cast<EntryFn>(g_base + kStreamAll);
		g_draw = reinterpret_cast<DrawFn>(g_base + kDraw);
		g_shutdown = reinterpret_cast<ShutdownFn>(g_base + kShutdown);
		g_clearA = reinterpret_cast<RequestFn>(g_base + kClearA);
		g_clearB = reinterpret_cast<RequestFn>(g_base + kClearB);
		g_release = reinterpret_cast<ReleaseFn>(g_base + kRelease);
		if (Goes(kAskCall, 0xE8, kAsk))
			g_ask = reinterpret_cast<AskFn>(g_base + kAsk);
		else
			log::Warn("minimap: the radar's streaming request is not where this build has it; a crossing blanks the radar for a moment");

		// the lands' supertiles: entries like the game's, numbered on the lattice + 1000 (their files' names)
		g_entries.assign(g_supertiles.size() * kEntry, 0);
		for (size_t k = 0; k < g_supertiles.size(); ++k)
		{
			Supertile& s = g_supertiles[k];
			s.entry = g_entries.data() + k * kEntry;
			ResetEntry(s.entry);
			*reinterpret_cast<int32_t*>(s.entry + 0x00) = s.row + kIndexBase;
			*reinterpret_cast<int32_t*>(s.entry + 0x04) = s.column + kIndexBase;
		}

		// each call goes through a jump of ours (jmp [rip+0]: within reach of the call's rel32)
		uint8_t* at = stubs;
		const auto stub = [&](const void* to) {
			uint8_t* start = at;
			const uint8_t jump[6] = {0xFF, 0x25, 0, 0, 0, 0};
			std::memcpy(at, jump, 6);
			const uint64_t address = reinterpret_cast<uint64_t>(to);
			std::memcpy(at + 6, &address, 8);
			at += 16;
			return reinterpret_cast<uintptr_t>(start);
		};
		const auto redirect = [&](uintptr_t site, uint8_t opcode, const void* to) {
			uint8_t b[5] = {opcode};
			const int32_t rel = static_cast<int32_t>(static_cast<intptr_t>(stub(to)) - static_cast<intptr_t>(g_base + site + 5));
			std::memcpy(&b[1], &rel, 4);
			Write(g_base + site, b, sizeof(b));
		};
		if (!g_supertiles.empty())
		{
			redirect(kSelectCall, 0xE8, reinterpret_cast<const void*>(&Select));
			redirect(kStreamAllCall, 0xE8, reinterpret_cast<const void*>(&StreamAll));
			redirect(kDrawAllCall, 0xE8, reinterpret_cast<const void*>(&DrawAll));
			redirect(kShutdownJump, 0xE9, reinterpret_cast<const void*>(&Shutdown));
		}
		if (!g_pictures.empty())
		{
			// (the trampoline: the five bytes, then on into the function)
			uint8_t* trampoline = at;
			std::memcpy(at, kPictureInitStart, sizeof(kPictureInitStart));
			at += sizeof(kPictureInitStart);
			const uint8_t jump[6] = {0xFF, 0x25, 0, 0, 0, 0};
			std::memcpy(at, jump, 6);
			const uint64_t back = g_base + kPictureInit + sizeof(kPictureInitStart);
			std::memcpy(at + 6, &back, 8);
			at = trampoline + 32;
			g_pictureInit = reinterpret_cast<InitFn>(trampoline);
			redirect(kPictureInit, 0xE9, reinterpret_cast<const void*>(&PictureInit));
			redirect(kFormatCall, 0xE8, reinterpret_cast<const void*>(&PictureName));
		}
		if (std::memcmp(reinterpret_cast<const void*>(g_base + kSquareStep), kSquareStepStart, sizeof(kSquareStepStart)) == 0)
			g_squareStep = reinterpret_cast<SquareStepFn>(g_base + kSquareStep);
		else
			log::Warn("minimap: the pause map's squares are not where this build has them; a sector switch may leave a picture behind");
		if (!g_lands.empty())
		{
			g_reach = reinterpret_cast<ReachFn>(g_base + kReach);
			for (const uintptr_t call : kReachCalls)
				redirect(call, 0xE8, reinterpret_cast<const void*>(&Reach));
		}
		FlushInstructionCache(GetCurrentProcess(), stubs, 256);
		log::Info("minimap: {} radar supertiles and {} pause map pictures of new land", g_supertiles.size(), g_pictures.size());
		return true;
	}

	void Suspend()
	{
		g_suspended = true;
		if (!g_stream)
			return;
		// (each request then as new: the next one looks its dictionary up again, in the files there then)
		for (Supertile& s : g_supertiles)
		{
			Release(s.entry);
			*reinterpret_cast<int32_t*>(s.entry + 0x0C) = -1;
			*reinterpret_cast<uint16_t*>(s.entry + 0x10) = 0x7F00;
		}
		std::lock_guard lock(g_ownMutex);
		for (auto& [land, own] : g_own)
			for (auto& t : own.tiles)
			{
				t->wanted = false;
				if (t->asked)
					Release(t->entry), t->asked = false;
			}
	}

	void Resume()
	{
		g_suspended = false;
		g_going.clear();
	}

	void Going(const std::vector<std::string>& lands)
	{
		g_going = lands;
		// (a sector switch: the lands put away take their files with them - their own supertiles let go and forgotten)
		std::lock_guard lock(g_ownMutex);
		for (const std::string& land : lands)
			if (const auto it = g_own.find(land); it != g_own.end())
			{
				for (auto& t : it->second.tiles)
					Release(t->entry);
				g_own.erase(it);
			}
	}

	std::string OwnName(const std::string& land, std::string_view name)
	{
		if (!g_ask || g_supertiles.empty())
			return {};
		int c = 0, r = 0;
		size_t from = 0, to = 0;
		if (!TileNumbers(name, c, r, from, to))
			return {};
		auto number = g_numbers.find(land);
		if (number == g_numbers.end())
			number = g_numbers.emplace(land, static_cast<int>(g_numbers.size()) + 1).first;
		// (the land's number in the column's: minimap_<number * 10000 + column + 1000>_<row + 1000>, a name no pack has)
		return std::format("{}{}_{}{}", name.substr(0, from), number->second * 10000 + c, r, name.substr(to));
	}

	void OwnTile(const std::string& land, std::string_view name)
	{
		int c = 0, r = 0;
		size_t from = 0, to = 0;
		const auto number = g_numbers.find(land);
		if (!TileNumbers(name, c, r, from, to) || number == g_numbers.end())
			return;
		auto t = std::make_unique<Own::Tile>();
		t->column = c - kIndexBase, t->row = r - kIndexBase;
		t->slot = SlotOf(std::format("minimap_{}_{}", number->second * 10000 + c, r));
		if (t->slot < 0)
			log::Warn("minimap: {}'s radar supertile {} {} not found under its own name", land, t->column, t->row);
		ResetEntry(t->entry);
		*reinterpret_cast<int32_t*>(t->entry + 0x00) = r;
		*reinterpret_cast<int32_t*>(t->entry + 0x04) = c;
		std::lock_guard lock(g_ownMutex);
		g_own[land].tiles.push_back(std::move(t));
	}

	void Arriving(const std::string& land, float dx, float dy)
	{
		// (its supertiles are registered over the next frames, as its data files are switched on: they join it then)
		if (!g_ask || g_supertiles.empty())
			return;
		std::lock_guard lock(g_ownMutex);
		Own& own = g_own[land];
		// (the lattice's columns east, its rows south)
		own.shift[0] = dx / (kSuperW / 6);
		own.shift[1] = -dy / (kSuperH / 6);
		own.arriving = true;
	}

	bool LetTilesGo(const std::string& land)
	{
		std::lock_guard lock(g_ownMutex);
		const auto it = g_own.find(land);
		if (it == g_own.end())
			return true;
		Own& own = it->second;
		own.going = true; // (none wanted any more: the stream lets each go)
		if (std::any_of(own.tiles.begin(), own.tiles.end(), [](const auto& t) { return t->asked || t->entry[0x08] || t->entry[0x71]; }))
		{
			own.quiet = 0;
			return false;
		}
		if (++own.quiet < 3)
			return false;
		g_own.erase(it);
		return true;
	}

	bool LetGo(const std::vector<int32_t>& slots)
	{
		if (!g_squareStep || slots.empty())
			return true;
		const auto* header = reinterpret_cast<const uint8_t*>(g_base + kSquares);
		auto* squares = *reinterpret_cast<uint8_t* const*>(header);
		const uint32_t count = *reinterpret_cast<const uint32_t*>(header + 8);
		if (!squares || count > 4096)
			return true;
		bool none = true;
		for (uint32_t i = 0; i < count; ++i)
		{
			uint8_t* square = squares + static_cast<size_t>(i) * 16;
			if (*reinterpret_cast<const int32_t*>(square + 4) == 0 ||
			    std::find(slots.begin(), slots.end(), *reinterpret_cast<const int32_t*>(square)) == slots.end())
				continue;
			none = false;
			g_squareStep(square, 0, 0, "");
		}
		return none;
	}
}
