#include "seamless.hpp"

#include <Windows.h>

#include <algorithm>
#include <array>
#include <charconv>
#include <chrono>
#include <cstring>
#include <deque>
#include <format>
#include <map>
#include <mutex>
#include <set>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

#include <MinHook.h>

#include "../convert/resource.hpp"
#include "../debug/profile.hpp"
#include "../log.hpp"
#include "../paths.hpp"
#include "../pattern.hpp"
#include "content.hpp"
#include "dlcpacks.hpp"
#include "landfiles.hpp"
#include "minimap.hpp"
#include "natives.hpp"

namespace loader::game::seamless
{
	namespace
	{
		using Clock = std::chrono::steady_clock;
		double Since(Clock::time_point t)
		{
			return std::chrono::duration<double, std::milli>(Clock::now() - t).count();
		}

		constexpr uint32_t kGroupStartup = 0x5BF6578F, kGroupMapSp = 0x578F99E2;
		// 0xC39B00 (a change set's data - its record +8 -, &list, -1, 0, true): the data files it enables, an atArray (+0 the
		// entries, +8 how many, u16) of 24-byte entries (+0 the data file, +8 its kind, +0xC on); 0xC39440 (an entry): one of
		// them enabled - kind 0 an archive (mounted, its files registered), 1 a meta file or archetype file asked for, 4
		// "everything asked for loaded now" (0x4D5110 -> 0xCF03F0: left out, they load as the rest of the world does). Round
		// them the map path sets the registration's pack flag (0xCE3310, the pack's +0xFC) - as here - and its state 2, under
		// which each archive's loaded objects are dropped first (0xCEDB50: new files, none).
		// Switched off the same way (0xC2F4D0: the list with -1, 0xFF, false, each entry's +0xC flipped and 0xC39440 called,
		// the last first): an archive's (0xC391B0) unregisters its files' handles (0xCE3BB0: every object whose handle is the
		// archive's - an overlay's gets back the one it took over, if any, else none) and closes it.
		constexpr uintptr_t kListDataFiles = 0xC39B00, kEnableDataFile = 0xC39440, kSetPackFlag = 0xCE3310, kFree = 0x18D0;
		// 0xC2D150 (a pack): its content.xml's change sets read into the data file manager's table on its first execution
		// (the pack's +0x118 bit 0 then set; +0x100 must be 2, mounted) - the map path calls it first
		constexpr uintptr_t kReadChangeSets = 0xC2D150;
		using ReadChangeSetsFn = void (*)(void* pack);
		using ListFn = void (*)(void* data, void* list, int32_t, int32_t, uint8_t);
		using EnableFn = uint64_t (*)(void* entry);
		using SetPackFlagFn = void (*)(uint32_t);
		using FreeFn = void (*)(void*);
		struct List
		{
			uint8_t* entries = nullptr;
			uint16_t count = 0, capacity = 0;
		};

		// 0xC272A0 (manager, pack, &group, &change set, flags - 1 executed, 2 with its loading screen, 4 the content's refresh):
		// a change set's own path (0xC27740), here with flags 1 and these of its calls left out: the waits for the other
		// threads (0x127A3A0, 0xCEECD0, 0x926EE0 twice), the world's resets (0x3A6370, 0x112D1F0, 0x1D94BB0 - each ped's
		// navmesh tracker -, 0xC527D0), its data files (done already, 0xC2E0B0 - the map branch's and the others'),
		// "everything asked for loaded now" (0xCF03F0) and the two box trees made again (vtable calls: done in frames of their
		// own).
		// 0xC274B0 (manager, pack, &group, &change set, -1, flags 1): the way back - the manager's record of it set not
		// executed, then 0xC27E80: the map branch (the same waits and resets; a load scene round the player, 0xC2F300; its
		// data files off, 0xC2F4D0 - done already, over frames; the map change data undone, 0xC0F420) and the change set
		// taken off the pack's executed list.
		constexpr uintptr_t kExecuteSet = 0xC272A0, kRevertSet = 0xC274B0;
		using ExecuteSetFn = void (*)(void* manager, void* pack, const uint32_t* group, const uint32_t* set, uint32_t flags);
		using RevertSetFn = void (*)(void* manager, void* pack, const uint32_t* group, const uint32_t* set, int32_t a, uint32_t flags);
		struct Site
		{
			uintptr_t at;
			uint8_t size;
			uintptr_t target; // (a call rel32: where it goes)
			uint8_t bytes[3]; // (a call through a vtable: its bytes)
		};
		// (the map change data's 0xC0F420, both ways: at its end the streaming objects of the change set's files looked for and
		// marked for loading again - 0xCEA720 twice, through all the objects for each archive: 190 ms for a land's first set)
		constexpr Site kExecuteLeftOut[] = {{0xC27B8F, 5, 0x3A6370}, {0xC27B94, 5, 0x127A3A0}, {0xC27BA0, 5, 0xCEECD0}, {0xC27BB4, 5, 0x926EE0},
		    {0xC27BBB, 5, 0x112D1F0}, {0xC27BC5, 5, 0x1D94BB0}, {0xC27C45, 5, 0xC527D0}, {0xC27C63, 5, 0x926EE0}, {0xC27C6B, 5, 0xC2E0B0},
		    {0xC27D48, 5, 0xC2E0B0}, {0xC27E44, 5, 0xCF03F0}, {0xC27DAD, 3, 0, {0xFF, 0x50, 0x30}}, {0xC27DBE, 3, 0, {0xFF, 0x50, 0x10}},
		    {0xC0FD23, 5, 0xCEA720}, {0xC0FD30, 5, 0xCEA720}};
		constexpr Site kRevertLeftOut[] = {{0xC28008, 5, 0x3A6370}, {0xC2800D, 5, 0x127A3A0}, {0xC28019, 5, 0xCEECD0}, {0xC2802A, 5, 0x926EE0},
		    {0xC28031, 5, 0x112D1F0}, {0xC2803B, 5, 0x1D94BB0}, {0xC28099, 5, 0x4E290}, {0xC28145, 5, 0xC2F300}, {0xC281D4, 5, 0xC527D0},
		    {0xC281F6, 5, 0x926EE0}, {0xC28201, 5, 0xC2F4D0}, {0xC2808D, 5, 0xC2F4D0}, {0xC0FD23, 5, 0xCEA720}, {0xC0FD30, 5, 0xCEA720}};
		// An archetype file's request switched off (0xC3C110: its hold on the file let go, 0xCEBBA0, then 0xCEA4B0 with 2 - all
		// the streaming objects looked through for those depending on it, 10-30 ms, and the file unloaded there and then,
		// waiting for the render thread if anything holds it): the look-through left out - the land's maps that use it are
		// off and given back with it (landfiles::ReleaseSome, as nothing holds it any more).
		constexpr Site kTypesLeftOut[] = {{0xC3C28F, 5, 0xCEA4B0}};
		// the box trees: the map store's box streamer (+0x3ED91D0, vtable +0x30) and the collision store's (+0x3ED8E30, +0x10)
		constexpr uintptr_t kMapStreamer = 0x3ED91D0, kBoundsStreamer = 0x3ED8E30;

		// The streaming objects (+0x42F9E48: +0 8 bytes each - the handle, archive << 16 | its entry, then the flags: the low
		// two bits 0 not there, 1 loaded, 2 asked for, 3 loading; 0x08000000 a resource file, 0x02000000 a road region without
		// one; +0x158 the handles overlays took over, by object: buckets, +0x160 how many, nodes of the object, the old handle,
		// the next) of the road network's regions (the Paths module, its first object + region = y * 32 + x on 1024 m from
		// -16384; past 1024 the island's set) and of the navmeshes (NavMeshes, + y * 147 + x on 150 m squares from the runtime
		// origin, +0x42F6580; a file "navmesh[<x * 3>][<y * 3>]", research/phase0.md §61.8): each one file, its handle. A
		// land's overlay (its start-up change set) brings both, its map archives the road regions the game has no file for
		// (left to them, a land registered took those names over everywhere: a region loaded later had the other land's
		// roads, linked to nodes its neighbours did not have - the route look-up's crash, 0xF4A020). Its files are
		// registered one by one (0xCE3320: rcx the
		// streaming, rdx where the object's index goes - -1 refused -, r8 the file's name, r9d its handle; nine arguments): a
		// later land's file takes over a name the land there has (0xCE36C5, the old handle kept for when it goes - a chain
		// of them as lands come and go) and for a loaded object first looks through all the objects for those depending on
		// it (0xCEA4B0 with 2: ~5 ms each). So every one of them is refused here, as the game refuses (rdx -1), its handle
		// taken down, and the objects' handles set by place here: a land's in the part of the world it is streamed in, the
		// game's own file - the one the first overlay took over - where no land there has one.
		constexpr uintptr_t kStreaming = 0x42F9E48, kNavOrigin = 0x42F6580, kRegisterFile = 0xCE3320;
		constexpr uintptr_t kPathsVtable = 0x2646520, kNavMeshesVtable = 0x262DBA0;
		constexpr uint32_t kNavWidth = 147;
		// the archives (+0x2C075B0: one a handle's archive number, +0x88 its path, "dlc_<pack>:/...")
		constexpr uintptr_t kArchives = 0x2C075B0;
		using RegisterFileFn = uint64_t (*)(void*, uint32_t*, const char*, uint32_t, uint64_t, uint64_t, uint64_t, uint64_t, uint64_t);
		RegisterFileFn g_registerFile = nullptr;
		bool g_installed = false; // (the sites as the build has them, the registration hooked)

		// the Paths' and NavMeshes' objects: their first, their end
		struct Ranges
		{
			uint32_t paths = 0, pathsEnd = 0, navs = 0, navsEnd = 0;
			uint8_t *pathsModule = nullptr, *navsModule = nullptr;
		} g_ranges;

		Ranges FindRanges()
		{
			const auto base = pattern::Module::Main().base;
			const auto* streaming = reinterpret_cast<const uint8_t*>(base + kStreaming);
			const auto* ranges = *reinterpret_cast<const uint8_t* const*>(streaming + 0x1A0);
			const uint16_t modules = *reinterpret_cast<const uint16_t*>(streaming + 0x1A8);
			Ranges r;
			for (uint16_t m = 0; ranges && m < modules; ++m)
			{
				const auto* module = *reinterpret_cast<const uint8_t* const*>(ranges + m * 16);
				if (!module)
					continue;
				const uintptr_t vt = *reinterpret_cast<const uintptr_t*>(module) - base;
				const uint32_t first = *reinterpret_cast<const uint32_t*>(module + 8), end = *reinterpret_cast<const uint32_t*>(ranges + m * 16 + 8);
				if (vt == kPathsVtable)
					r.paths = first, r.pathsEnd = end, r.pathsModule = const_cast<uint8_t*>(module);
				else if (vt == kNavMeshesVtable)
					r.navs = first, r.navsEnd = end, r.navsModule = const_cast<uint8_t*>(module);
			}
			return r;
		}

		uint32_t* Entries()
		{
			return *reinterpret_cast<uint32_t**>(pattern::Module::Main().base + kStreaming);
		}

		// a road network region's or navmesh's object from its file's name ("nodes<n>", "navmesh[<x>][<y>]", a path before it
		// or not), or 0
		uint32_t ObjectOf(std::string_view name)
		{
			if (const size_t slash = name.find_last_of("/\\"); slash != std::string_view::npos)
				name.remove_prefix(slash + 1);
			const auto number = [&](size_t at, uint32_t& n) {
				const auto [end, ec] = std::from_chars(name.data() + at, name.data() + name.size(), n);
				return ec == std::errc() ? static_cast<size_t>(end - name.data()) : std::string_view::npos;
			};
			uint32_t n = 0, x = 0, y = 0;
			if (name.starts_with("nodes") && number(5, n) != std::string_view::npos)
				return n < 1024 ? g_ranges.paths + n : 0;
			if (name.starts_with("navmesh["))
			{
				const size_t at = number(8, x);
				if (at != std::string_view::npos && name.substr(at).starts_with("][") && number(at + 2, y) != std::string_view::npos)
					return g_ranges.navs + (y / 3) * kNavWidth + x / 3;
			}
			return 0;
		}

		// whether a region's or navmesh's object is one of ours (not the island's set) and its square meets `rect` (min x, min
		// y, max x, max y)
		bool Ours(uint32_t object)
		{
			return (object >= g_ranges.paths && object < g_ranges.paths + 1024) || (object >= g_ranges.navs && object < g_ranges.navsEnd);
		}
		bool Meets(uint32_t object, const float* rect)
		{
			float x0 = 0, y0 = 0, size = 0;
			if (object < g_ranges.pathsEnd)
			{
				const uint32_t n = object - g_ranges.paths;
				x0 = (n % 32) * 1024.0f - 16384, y0 = (n / 32) * 1024.0f - 16384, size = 1024;
			}
			else
			{
				const uint32_t n = object - g_ranges.navs;
				const auto* origin = reinterpret_cast<const float*>(pattern::Module::Main().base + kNavOrigin);
				x0 = origin[0] + (n % kNavWidth) * 150.0f, y0 = origin[1] + (n / kNavWidth) * 150.0f, size = 150;
			}
			return x0 + size > rect[0] && x0 < rect[2] && y0 + size > rect[1] && y0 < rect[3];
		}

		// The lands (sectors' packs) and which land an archive is (its path's device, "dlc_<land>:"), "" none
		std::set<std::string> g_lands;
		std::string LandOfHandle(uint32_t handle)
		{
			if (!handle || (handle & 0xC0000000) == 0x80000000)
				return "";
			const auto base = pattern::Module::Main().base;
			const auto archive = *reinterpret_cast<const uint8_t* const*>(base + kArchives + static_cast<uintptr_t>(handle >> 16) * 8);
			const char* path = archive ? *reinterpret_cast<const char* const*>(archive + 0x88) : nullptr;
			if (!path || std::strncmp(path, "dlc_", 4) != 0)
				return "";
			const char* colon = std::strchr(path, ':');
			const std::string land = colon ? std::string(path + 4, colon) : std::string();
			return g_lands.contains(land) ? land : "";
		}

		// Each land's road region and navmesh files the game has (object -> handle; its overlay's and its map archives'): taken
		// down as they are registered (all refused), or read from the objects for the land the game registered itself at the
		// start; kept while it is registered.
		std::map<std::string, std::unordered_map<uint32_t, uint32_t>> g_overlays;
		// What each object had before any land's overlay (the game's own file, 0 none): read once
		std::vector<uint32_t> g_base;
		std::string* g_capture = nullptr; // (the land whose data files are being registered)
		DWORD g_captureThread = 0;

		// The radar's supertiles of each land registered (minimap_<column>_<row>.ydd, name hash -> handle): the same names in
		// every land at a place, the first registered keeps the name - the next one's is not taken -, so the land crossed into
		// is given its own at the handover (the radar let go of them first)
		std::map<std::string, std::unordered_map<uint32_t, uint32_t>> g_radar;
		std::string* g_registering = nullptr; // (the land whose data files are being switched on)

		bool RadarTile(std::string_view name, uint32_t& hash)
		{
			if (const size_t slash = name.find_last_of("/\\"); slash != std::string_view::npos)
				name.remove_prefix(slash + 1);
			if (name.ends_with(".ydd"))
				name.remove_suffix(4);
			if (!name.starts_with("minimap_"))
				return false;
			const std::string_view rest = name.substr(8);
			const size_t under = rest.find('_');
			if (under == 0 || under == std::string_view::npos || under + 1 >= rest.size())
				return false;
			for (size_t k = 0; k < rest.size(); ++k)
				if (k != under && (rest[k] < '0' || rest[k] > '9'))
					return false;
			hash = convert::Joaat(name);
			return true;
		}

		uint64_t RegisterFileHook(void* a, uint32_t* index, const char* name, uint32_t handle, uint64_t e, uint64_t f, uint64_t g, uint64_t h, uint64_t i)
		{
			if (name && GetCurrentThreadId() == g_captureThread)
			{
				if (g_capture)
					if (const uint32_t object = ObjectOf(name); object && Ours(object))
					{
						g_overlays[*g_capture][object] = handle;
						*index = 0xFFFFFFFF;
						return reinterpret_cast<uint64_t>(index);
					}
				if (uint32_t tile = 0; g_registering && RadarTile(name, tile))
				{
					// (under a name of the land's own: the land there keeps the pack's, the radar streams this one before the
					// crossing - game/minimap.hpp)
					if (const std::string own = minimap::OwnName(*g_registering, name); !own.empty())
					{
						static std::deque<std::string> kept; // (the names as registered, kept)
						kept.push_back(own);
						const uint64_t result = g_registerFile(a, index, kept.back().c_str(), handle, e, f, g, h, i);
						minimap::OwnTile(*g_registering, name);
						return result;
					}
					g_radar[*g_registering][tile] = handle;
				}
			}
			return g_registerFile(a, index, name, handle, e, f, g, h, i);
		}

		// the overlays' map of the handles they took over: the one an object had before, or 0
		uint32_t TakenOver(uint32_t object)
		{
			const auto* streaming = reinterpret_cast<const uint8_t*>(pattern::Module::Main().base + kStreaming);
			const auto* buckets = *reinterpret_cast<const uint8_t* const* const*>(streaming + 0x158);
			const uint16_t count = *reinterpret_cast<const uint16_t*>(streaming + 0x160);
			if (!buckets || !count)
				return 0;
			for (const uint8_t* node = buckets[object % count]; node; node = *reinterpret_cast<const uint8_t* const*>(node + 8))
				if (*reinterpret_cast<const uint32_t*>(node) == object)
					return *reinterpret_cast<const uint32_t*>(node + 4);
			return 0;
		}

		// (once: every object's own file, and the overlay of the land the game registered itself)
		void ReadBase()
		{
			if (!g_base.empty())
				return;
			const uint32_t* entries = Entries();
			g_base.resize(g_ranges.navsEnd - g_ranges.paths);
			for (uint32_t o = g_ranges.paths; o < g_ranges.navsEnd; ++o)
			{
				if (!Ours(o))
					continue;
				const uint32_t handle = entries[static_cast<size_t>(o) * 2];
				const std::string land = LandOfHandle(handle);
				if (land.empty())
					g_base[o - g_ranges.paths] = handle;
				else
				{
					g_overlays[land][o] = handle;
					const uint32_t old = TakenOver(o);
					g_base[o - g_ranges.paths] = LandOfHandle(old).empty() ? old : 0;
				}
			}
		}

		// a land's overlay as the objects have it (the land the game registered at the start: read with the base)
		const std::unordered_map<uint32_t, uint32_t>& OverlayOf(const std::string& land)
		{
			ReadBase();
			return g_overlays[land];
		}

		// An object's file set (0: none) - not while it is loaded or on its way (false: later); the flags as the
		// registration leaves them (0xCE35D0: the low word kept, a resource's bit; a road region without a file is a
		// placeholder).
		bool SetFile(uint32_t object, uint32_t handle)
		{
			uint32_t* e = Entries() + static_cast<size_t>(object) * 2;
			if (e[0] == handle)
				return true;
			if (e[1] & 3)
				return false;
			const bool region = object < g_ranges.pathsEnd;
			e[0] = handle;
			e[1] = (e[1] & 0xFFFF) | (handle ? 0x08000000u : region ? 0x02000000u : 0u);
			return true;
		}

		// Objects whose file is to change once they are no longer loaded (the old place's, after a crossing): object -> handle
		std::map<uint32_t, uint32_t> g_deferred;
		Clock::time_point g_deferredSince;

		// One of them unloaded by the streaming engine (RemoveObject, +0xCE4CD0) when nothing holds it - no hold flags, its
		// module's count of holders (vtable +0xC8) none - and its square is not round the player: the road network and the
		// path server keep what they once loaded till they want room, the old place's for good (one the game wants is
		// loaded again - with its new file). Whether it was. (Not only far off: a land taken back as the player turned
		// round left a region 2 km from them loaded, and its removal - and with it the next land's coming - waited on it.)
		constexpr uintptr_t kRemoveObject = 0xCE4CD0;
		constexpr float kUnloadFar = 1200; // (m from the player)
		bool Unload(uint32_t object, float px, float py)
		{
			uint32_t* e = Entries() + static_cast<size_t>(object) * 2;
			const bool region = object < g_ranges.pathsEnd;
			uint8_t* module = region ? g_ranges.pathsModule : g_ranges.navsModule;
			if (!module || (e[1] & 3) != 1 || (e[1] & 0xFFFC))
				return false;
			const float round[4] = {px - kUnloadFar, py - kUnloadFar, px + kUnloadFar, py + kUnloadFar};
			if (Meets(object, round))
				return false;
			const auto refs = *reinterpret_cast<int32_t (**)(uint8_t*, int32_t)>(*reinterpret_cast<uintptr_t*>(module) + 0xC8);
			const int32_t slot = static_cast<int32_t>(object - (region ? g_ranges.paths : g_ranges.navs));
			if (refs(module, slot) != 0)
				return false;
			const auto base = pattern::Module::Main().base;
			reinterpret_cast<uint8_t (*)(void*, uint32_t, uint32_t)>(base + kRemoveObject)(reinterpret_cast<void*>(base + kStreaming), object, 0);
			return (e[1] & 3) == 0;
		}

		// the objects meeting `rect` (or all but those meeting `except`) given `land`'s files, the game's own where it has none:
		// how many changed and how many wait for being unloaded
		std::pair<size_t, size_t> Hand(const std::string& land, const float* rect, const float* except)
		{
			ReadBase();
			const auto& theirs = OverlayOf(land);
			size_t changed = 0, waiting = 0;
			for (uint32_t o = g_ranges.paths; o < g_ranges.navsEnd; ++o)
			{
				if (!Ours(o) || (rect && !Meets(o, rect)) || (except && Meets(o, except)))
					continue;
				const auto it = theirs.find(o);
				const uint32_t handle = it != theirs.end() ? it->second : g_base[o - g_ranges.paths];
				const uint32_t was = Entries()[static_cast<size_t>(o) * 2];
				if (was == handle)
				{
					g_deferred.erase(o);
					continue;
				}
				if (SetFile(o, handle))
					++changed, g_deferred.erase(o);
				else
				{
					if (g_deferred.empty())
						g_deferredSince = Clock::now();
					g_deferred[o] = handle, ++waiting;
				}
			}
			return {changed, waiting};
		}

		// the left-out calls made nothing (on) or put back
		void LeaveOut(const Site* sites, size_t count, bool on)
		{
			static std::map<uintptr_t, std::array<uint8_t, 5>> saved;
			static constexpr uint8_t kNop5[] = {0x0F, 0x1F, 0x44, 0x00, 0x00}, kNop3[] = {0x0F, 0x1F, 0x00};
			const auto base = pattern::Module::Main().base;
			for (size_t i = 0; i < count; ++i)
			{
				auto* at = reinterpret_cast<uint8_t*>(base + sites[i].at);
				DWORD old = 0;
				VirtualProtect(at, sites[i].size, PAGE_EXECUTE_READWRITE, &old);
				if (on)
				{
					std::memcpy(saved[sites[i].at].data(), at, sites[i].size);
					std::memcpy(at, sites[i].size == 5 ? kNop5 : kNop3, sites[i].size);
				}
				else
					std::memcpy(at, saved[sites[i].at].data(), sites[i].size);
				VirtualProtect(at, sites[i].size, old, &old);
				FlushInstructionCache(GetCurrentProcess(), at, sites[i].size);
			}
		}

		bool SitesAsBuilt(const Site* sites, size_t count)
		{
			const auto base = pattern::Module::Main().base;
			for (size_t i = 0; i < count; ++i)
			{
				const Site& s = sites[i];
				const auto* at = reinterpret_cast<const uint8_t*>(base + s.at);
				int32_t rel = 0;
				std::memcpy(&rel, at + 1, 4);
				if (s.size == 5 ? at[0] != 0xE8 || s.at + 5 + rel != s.target : std::memcmp(at, s.bytes, 3) != 0)
				{
					log::Warn("seamless: the content manager's code at {:#x} differs from the build this was made for; crossing without a cut is off", s.at);
					return false;
				}
			}
			return true;
		}

		std::filesystem::path PackOf(const std::string& land)
		{
			return paths::LandDir(land) / L"dlc.rpf";
		}

		// a land's change sets of a group, each with its data files (the list as 0xC39B00 makes it: to switch on, `on`, or
		// off - the last first)
		struct ChangeSet
		{
			uint32_t group = 0, hash = 0;
			std::vector<std::array<uint8_t, 24>> entries;
		};
		bool ListSets(uint8_t* pack, uint32_t group, bool on, std::vector<ChangeSet>& out, std::string& error)
		{
			const auto base = pattern::Module::Main().base;
			const auto list = reinterpret_cast<ListFn>(base + kListDataFiles);
			uint16_t count = 0;
			const uint32_t* hashes = content::GroupChangeSets(pack, group, count);
			for (uint16_t i = 0; hashes && i < count; ++i)
			{
				uint8_t* record = content::ChangeSetRecord(hashes[i]);
				if (!record)
					return error = std::format("no record of change set {:08x}", hashes[i]), false;
				List l;
				if (on)
					list(record + 8, &l, -1, 0, 1);
				else
					list(record + 8, &l, -1, 0xFF, 0);
				ChangeSet cs{group, hashes[i]};
				for (uint16_t e = 0; e < l.count; ++e)
				{
					std::array<uint8_t, 24> entry{};
					std::memcpy(entry.data(), l.entries + static_cast<size_t>(e) * 24, 24);
					cs.entries.push_back(entry);
				}
				if (l.capacity && l.entries)
					reinterpret_cast<FreeFn>(base + kFree)(l.entries);
				if (!on)
					std::reverse(cs.entries.begin(), cs.entries.end());
				out.push_back(std::move(cs));
			}
			return true;
		}

		// a data file entry's file name
		std::string NameOf(const uint8_t* entry)
		{
			const auto* file = *reinterpret_cast<const char* const*>(entry);
			char name[96] = "?";
			if (file)
				strncpy_s(name, file, _TRUNCATE);
			return name;
		}

		// The work, one job at a time, a step a frame:
		//  - Register a land (its pack mounted, its change sets' data files on over frames - its overlay's files taken down,
		//    its maps' registered -, each set processed, its maps streamed only in the band - the land there's not there -,
		//    the regions and navmeshes in the band given its files);
		//  - Handover (the player now in the land registered: it everywhere, the one there nowhere - maps and regions and
		//    navmeshes -, then the one there removed);
		//  - Cancel (a land registered for a crossing that did not come: off again, the land there back in the band, removed);
		//  - Remove a land (its files given back over frames, its data files off, its change sets taken back - the world not
		//    cleared).
		enum class Kind { Register, Handover, Cancel, Remove };
		struct Job
		{
			Kind kind;
			std::string land, other;
			float band[4]{};
			int step = 0;
			uint8_t* pack = nullptr;
			std::vector<ChangeSet> sets;
			size_t set = 0, entry = 0;
			Clock::time_point began = Clock::now(), since = Clock::now();
			bool waiting = false; // (this frame's step only waited for something else: another job may go first)
		};
		// (lands whose files could not all be given back: their archives and change sets left as they are - not crossed into
		// without a cut again this session; the switch with the screen faded takes them back as the game does)
		std::set<std::string> g_stuck;
		std::deque<Job> g_jobs;
		std::vector<std::string> g_log; // (each step's work and time: Status)
		std::string g_registered;       // (the land the last Register finished with)
		constexpr double kBudget = 2.5; // (ms of data files or released files a frame, at least one)
		bool g_profilePost = false;     // (research: the change sets' processing sampled)
		uintptr_t g_profileFrom = 0, g_profileTo = 0; // (research: the data files' enabling sampled, by its calls in this range)
		constexpr float kAll[4] = {-16000, -16000, 16000, 16000};

		// the frames' times while recording (ms), and the steps done in each
		bool g_recording = false;
		Clock::time_point g_lastFrame;
		struct Frame
		{
			Clock::time_point start, end;
			std::string work;
			double ms() const { return std::chrono::duration<double, std::milli>(end - start).count(); }
		};
		std::vector<Frame> g_frames;
		std::string g_frameWork;
		std::mutex g_noteMutex; // (notes may come from other threads)

		// Research, while the frames are recorded: the physics' broad phase taking objects in (0x1059E40: rcx the sweep and
		// prune, rdx their boxes' low corners, r8 the high ones - 16 bytes each -, r9 their handles, the fifth a u16 how
		// many; their ends sorted into place along each axis, 0x105A8E0) - how many and where, when it takes long.
		constexpr uintptr_t kBroadPhaseAdd = 0x1059E40;
		constexpr uint8_t kBroadPhaseAddBytes[] = {0x41, 0x57, 0x41, 0x56, 0x41, 0x55, 0x41, 0x54, 0x56, 0x57, 0x55, 0x53, 0x48, 0x81, 0xEC, 0x48, 0x01, 0x00, 0x00};
		using BroadPhaseAddFn = void (*)(void*, const float*, const float*, void*, uint64_t, uint64_t, uint64_t, uint64_t);
		BroadPhaseAddFn g_broadPhaseAdd = nullptr;
		void BroadPhaseAddHook(void* sap, const float* low, const float* high, void* handles, uint64_t count, uint64_t a, uint64_t b, uint64_t c)
		{
			if (!g_recording)
				return g_broadPhaseAdd(sap, low, high, handles, count, a, b, c);
			const auto t = Clock::now();
			g_broadPhaseAdd(sap, low, high, handles, count, a, b, c);
			const double ms = std::chrono::duration<double, std::milli>(Clock::now() - t).count();
			if (ms < 3)
				return;
			const uint16_t n = static_cast<uint16_t>(count);
			float x0 = 1e9f, y0 = 1e9f, x1 = -1e9f, y1 = -1e9f;
			size_t sizes[5]{}; // (by the longest side: < 1 m, < 3, < 6, < 20, more)
			std::string some;
			for (uint16_t i = 0; i < n; ++i)
			{
				const float* l = low + i * 4;
				const float* h = high + i * 4;
				x0 = std::min(x0, l[0]), y0 = std::min(y0, l[1]);
				x1 = std::max(x1, h[0]), y1 = std::max(y1, h[1]);
				const float side = std::max({h[0] - l[0], h[1] - l[1], h[2] - l[2]});
				++sizes[side < 1 ? 0 : side < 3 ? 1 : side < 6 ? 2 : side < 20 ? 3 : 4];
				if (i < 4)
					some += std::format(" ({:.1f}, {:.1f}, {:.1f}) {:.1f}x{:.1f}x{:.1f} h{:08x}", l[0], l[1], l[2], h[0] - l[0], h[1] - l[1], h[2] - l[2],
					    static_cast<const uint32_t*>(handles)[i]);
			}
			Note(std::format("the physics' broad phase took {} in ({:.1f} ms; x {:.0f}..{:.0f}, y {:.0f}..{:.0f}; sides <1 m {}, <3 {}, <6 {}, <20 {}, more {};{})", n,
			    ms, x0, x1, y0, y1, sizes[0], sizes[1], sizes[2], sizes[3], sizes[4], some));
		}

		void Say(const std::string& line)
		{
			g_log.push_back(line);
			if (g_log.size() > 400)
				g_log.erase(g_log.begin(), g_log.begin() + 100);
		}

		void RebuildTree(uintptr_t streamer, size_t slot)
		{
			auto* s = reinterpret_cast<uint8_t*>(pattern::Module::Main().base + streamer);
			(*reinterpret_cast<void (**)(uint8_t*)>(*reinterpret_cast<uintptr_t*>(s) + slot))(s);
		}

		// One step of switching data files on or off (the pack flag round each switched on, as the map path sets it);
		// "" while there are more
		enum class Which { All, Archives, Others };
		std::string DataFiles(Job& j, bool on, Clock::time_point t, Which which = Which::All)
		{
			const auto base = pattern::Module::Main().base;
			const auto enable = reinterpret_cast<EnableFn>(base + kEnableDataFile);
			const auto setPackFlag = reinterpret_cast<SetPackFlagFn>(base + kSetPackFlag);
			ChangeSet& cs = j.sets[j.set];
			int done = 0;
			std::string slow;
			while (j.entry < cs.entries.size() && (done == 0 || Since(t) < kBudget))
			{
				auto& entry = cs.entries[j.entry++];
				const uint32_t kind = *reinterpret_cast<const uint32_t*>(entry.data() + 8);
				if (kind == 4 || (which == Which::Archives && kind != 0) || (which == Which::Others && kind == 0))
					continue;
				std::string land = j.land;
				const auto t1 = Clock::now();
				if (on)
				{
					// (its map archives' too: the road regions the game has no file for are there, not in the overlay)
					g_capture = &land;
					g_registering = &land;
					g_captureThread = GetCurrentThreadId();
					setPackFlag(*reinterpret_cast<const uint32_t*>(j.pack + 0xFC));
					std::string samples;
					if (g_profileFrom)
						samples = " [" + debug::profile::Profiled(g_profileFrom, g_profileTo, [&] { enable(entry.data()); }) + "]";
					else
						enable(entry.data());
					setPackFlag(0);
					g_capture = nullptr;
					g_registering = nullptr;
					if (const double ms = Since(t1); ms > 5)
						slow += std::format("; {} kind {} {:.1f} ms{}", NameOf(entry.data()), *reinterpret_cast<const uint32_t*>(entry.data() + 8), ms, samples);
				}
				else
				{
					entry[0xC] ^= 1;
					LeaveOut(kTypesLeftOut, std::size(kTypesLeftOut), true);
					enable(entry.data());
					LeaveOut(kTypesLeftOut, std::size(kTypesLeftOut), false);
					if (const double ms = Since(t1); ms > 5)
						slow += std::format("; {} off {:.1f} ms", NameOf(entry.data()), ms);
				}
				++done;
			}
			return std::format("{} data file(s) of set {} {}{}", done, j.set, on ? "on" : "off", slow);
		}

		// One step of a job; true when the job is done.
		bool Step(Job& j, std::string& work)
		{
			const auto base = pattern::Module::Main().base;
			const auto t = Clock::now();
			const auto next = [&](int step) { j.step = step, j.since = Clock::now(); };
			switch (j.kind)
			{
			case Kind::Register:
				switch (j.step)
				{
				case 0: // the overlay's data files on, a frame's worth at a time
				case 2: // the maps'
					work = DataFiles(j, true, t);
					if (j.entry >= j.sets[j.set].entries.size())
						next(j.step + 1);
					return false;
				case 1: // each set processed
				case 3:
				{
					const ChangeSet& cs = j.sets[j.set];
					const uint32_t group = cs.group, set = cs.hash;
					LeaveOut(kExecuteLeftOut, std::size(kExecuteLeftOut), true);
					const auto run = [&] { reinterpret_cast<ExecuteSetFn>(base + kExecuteSet)(content::ManagerRecord(), j.pack, &group, &set, 1); };
					std::string samples;
					if (g_profilePost)
						samples = " [" + debug::profile::Profiled(0xC27740, 0xC27E80, run) + "]";
					else
						run();
					LeaveOut(kExecuteLeftOut, std::size(kExecuteLeftOut), false);
					work = std::format("set {} ({:08x}) processed{}", j.set, set, samples);
					const bool overlayDone = j.step == 1 && (j.set + 1 >= j.sets.size() || j.sets[j.set + 1].group != kGroupStartup);
					j.entry = 0;
					if (overlayDone)
						++j.set, next(4);
					else if (++j.set < j.sets.size())
						next(j.step - 1);
					else
						next(5);
					return false;
				}
				case 4:
				{
					// the regions and navmeshes in the band given the land's files (those loaded wait)
					const auto [changed, waiting] = Hand(j.land, j.band, nullptr);
					work = std::format("{} files of {}'s overlay taken down; in the band {} objects given its files, {} wait for being unloaded",
					    g_overlays[j.land].size(), j.land, changed, waiting);
					next(j.set < j.sets.size() ? 2 : 5);
					return false;
				}
				case 5: // the land's names, read on their own thread
					work = "its names being read";
					if (landfiles::NamesReady(j.land))
					{
						// (the road regions its map archives brought - registered after the overlay's were handed - given it
						// in the band too)
						const auto [changed, waiting] = Hand(j.land, j.band, nullptr);
						work = std::format("in the band {} more objects given its files, {} wait for being unloaded", changed, waiting);
						// (a land crossed into before: its collision files' boxes and flags as they were when it gave them back -
						// its registration does not set them again -, before its areas are switched)
						if (const size_t back = landfiles::PutBoxesBack(j.land))
							work += std::format("; {} collision boxes put back", back);
						next(6);
					}
					return false;
				case 6:
					work = std::format("{} of {} off", landfiles::SwitchArea(j.land, PackOf(j.land), kAll, false), j.land);
					next(7);
					return false;
				case 7:
					work = std::format("{} of {} on in the band", landfiles::SwitchArea(j.land, PackOf(j.land), j.band, true), j.land);
					next(8);
					return false;
				case 8:
					if (!j.other.empty())
						work = std::format("{} of {} off in the band", landfiles::SwitchArea(j.other, PackOf(j.other), j.band, false), j.other);
					next(j.other.empty() ? 10 : 9);
					return false;
				case 9:
				{
					// (the land there's maps in the band it still has loaded unloaded - switched off they stay in the world)
					const auto p = landfiles::UnloadMaps(j.other, PackOf(j.other), j.band, kBudget);
					work = std::format("{}'s maps in the band: {} unloaded, {} still loaded", j.other, p.released, p.loaded);
					if (p.done || Since(j.since) > 5000)
					{
						if (!p.done)
							log::Warn("seamless: {} of {}'s maps in the band still loaded after 5 s", p.loaded, j.other);
						next(10);
					}
					return false;
				}
				case 10:
					RebuildTree(kBoundsStreamer, 0x10);
					work = "the collision box tree made again";
					next(11);
					return false;
				case 11:
					RebuildTree(kMapStreamer, 0x30);
					work = "the map box tree made again";
					g_registered = j.land;
					return true;
				}
				return true;
			case Kind::Handover:
			case Kind::Cancel:
			{
				// Handover: `land` the land the player is in now, `other` the one they left; Cancel: `land` the one registered,
				// `other` the one there
				const bool handover = j.kind == Kind::Handover;
				switch (j.step)
				{
				case 0:
					work = handover ? std::format("{} of {} off", landfiles::SwitchArea(j.other, PackOf(j.other), kAll, false), j.other)
					                : std::format("{} of {} off in the band", landfiles::SwitchArea(j.land, PackOf(j.land), j.band, false), j.land);
					next(1);
					return false;
				case 1:
					work = handover ? std::format("{} of {} on", landfiles::SwitchArea(j.land, PackOf(j.land), kAll, true), j.land)
					                : std::format("{} of {} on in the band again", landfiles::SwitchArea(j.other, PackOf(j.other), j.band, true), j.other);
					next(2);
					return false;
				case 2:
					RebuildTree(kBoundsStreamer, 0x10);
					work = "the collision box tree made again";
					next(3);
					return false;
				case 3:
					RebuildTree(kMapStreamer, 0x30);
					work = "the map box tree made again";
					next(4);
					return false;
				case 4:
				{
					const auto [changed, waiting] = handover ? Hand(j.land, nullptr, j.band) : Hand(j.other, j.band, nullptr);
					work = std::format("{} regions/navmeshes given {}'s files, {} wait for being unloaded", changed, handover ? j.land : j.other, waiting);
					// (the radar's supertiles of the names the land there had first: the radar lets go of them all now - its
					// dictionaries the render thread's no more after a few frames -, then they are given the land's files)
					const bool tiles = handover && landfiles::DictionariesToGive(g_radar[j.land]) > 0;
					if (tiles)
						minimap::Suspend();
					j.entry = 0;
					next(tiles ? 5 : 7);
					return false;
				}
				case 5:
					work = "the radar let go of its supertiles";
					if (++j.entry >= 3)
						next(6);
					return false;
				case 6:
				{
					const size_t waiting = landfiles::GiveDictionaries(g_radar[j.land]);
					work = std::format("the radar's supertiles given {}'s files, {} still held", j.land, waiting);
					if (waiting == 0 || Since(j.since) > 5000)
					{
						if (waiting)
							log::Warn("seamless: {} of the radar's supertiles still held after 5 s: they show {}'s", waiting, j.other);
						minimap::Resume();
						next(7);
					}
					return false;
				}
				case 7:
					// then the land gone removed (a job of its own)
					g_jobs.push_back(Job{Kind::Remove, handover ? j.other : j.land});
					work = std::format("{} to be taken back", handover ? j.other : j.land);
					return true;
				}
				return true;
			}
			case Kind::Remove:
				switch (j.step)
				{
				case 0:
				{
					// its maps still loaded unloaded (switched off they stay in the world: the box streamer lets go only of
					// those it streams)
					const auto p = landfiles::UnloadMaps(j.land, PackOf(j.land), kAll, kBudget);
					work = std::format("{}'s maps: {} unloaded, {} still loaded", j.land, p.released, p.loaded);
					if (p.done || Since(j.since) > 20000)
					{
						if (!p.done)
							log::Warn("seamless: {} of {}'s maps still loaded after 20 s (their files are kept)", p.loaded, j.land);
						next(1);
					}
					else
						j.waiting = p.released == 0;
					return false;
				}
				case 1:
				{
					// its pause map pictures let go by the squares holding them (the radar draws them too, zoomed out), and its
					// radar supertiles under names of its own
					const bool tiles = minimap::LetTilesGo(j.land);
					const bool done = landfiles::LetPicturesGo(j.land, PackOf(j.land)) && tiles;
					work = std::format("{}'s pause map pictures and radar {}", j.land, done ? "let go" : "still held");
					if (done || Since(j.since) > 10000)
						next(2);
					else
						j.waiting = true;
					return false;
				}
				case 2:
				{
					// (once no region or navmesh has its files any more: the old place's unloaded as the game lets them go - the
					// path server's once no one is on them, not taken from under it)
					size_t theirs = 0;
					for (const auto& [object, handle] : g_deferred)
						theirs += LandOfHandle(Entries()[static_cast<size_t>(object) * 2]) == j.land;
					if (theirs)
					{
						work = std::format("{} region(s)/navmesh(es) still {}'s", theirs, j.land);
						j.waiting = true;
						return false;
					}
					std::string error;
					j.pack = content::PackRecord(convert::Joaat(j.land));
					j.sets.clear();
					if (!j.pack || !ListSets(j.pack, kGroupMapSp, false, j.sets, error) || !ListSets(j.pack, kGroupStartup, false, j.sets, error))
					{
						log::Warn("seamless: {}'s change sets not listed ({}); they stay as they are", j.land, error.empty() ? "no pack" : error);
						g_stuck.insert(j.land);
						return true;
					}
					// (the last change set first, as the game takes them back)
					std::reverse(j.sets.begin(), j.sets.end());
					j.set = 0, j.entry = 0;
					next(3);
					return false;
				}
				case 3:
				case 7:
					// its data files off: first all but the archives (its archetype files' requests let go - the game then
					// unloads them as it does any it no longer needs), then, once its files are given back, its archives (closed:
					// the archive taken away from under a loaded file left the file loaded with nothing in its store, and the
					// game's own clean-up stopped on it, +0x939323)
					if (j.set >= j.sets.size())
					{
						j.set = 0, j.entry = 0;
						next(j.step + 1);
						return false;
					}
					work = DataFiles(j, false, t, j.step == 3 ? Which::Others : Which::Archives);
					if (j.entry >= j.sets[j.set].entries.size())
						++j.set, j.entry = 0;
					return false;
				case 4:
				{
					// its files unloaded and given back over frames (what is still loaded after a minute kept)
					const bool last = Since(j.since) > 60000;
					const auto p = landfiles::ReleaseSome(j.land, PackOf(j.land), kBudget, last);
					work = std::format("{}'s files: {} given back, {} still loaded", j.land, p.released, p.loaded);
					if (!p.done)
					{
						j.waiting = p.released == 0;
						return false;
					}
					if (p.kept)
					{
						log::Warn("seamless: {}: {} of its files still held after a minute; its archives and change sets left as they are", j.land, p.kept);
						g_stuck.insert(j.land);
						g_overlays.erase(j.land);
						return true;
					}
					next(5);
					return false;
				}
				case 5:
					// the box trees made again without its slots (each a frame)
					RebuildTree(kBoundsStreamer, 0x10);
					work = "the collision box tree made again";
					next(6);
					return false;
				case 6:
					RebuildTree(kMapStreamer, 0x30);
					work = "the map box tree made again";
					next(7);
					return false;
				case 8:
				{
					// each change set taken back (the manager's record, the map change data, the pack's executed list)
					if (j.set >= j.sets.size())
						return true;
					const ChangeSet& cs = j.sets[j.set];
					const uint32_t group = cs.group, set = cs.hash;
					LeaveOut(kRevertLeftOut, std::size(kRevertLeftOut), true);
					reinterpret_cast<RevertSetFn>(base + kRevertSet)(content::ManagerRecord(), j.pack, &group, &set, -1, 1);
					LeaveOut(kRevertLeftOut, std::size(kRevertLeftOut), false);
					work = std::format("set {} ({:08x}) taken back", j.set, set);
					if (++j.set < j.sets.size())
						return false;
					// (its map caches read again at its next registration)
					std::vector<uint32_t> hashes;
					for (const ChangeSet& c : j.sets)
						hashes.push_back(c.hash);
					work += std::format("; {} of its caches off the list of those read", landfiles::ForgetCaches(j.land, hashes));
					g_overlays.erase(j.land);
					if (g_registered == j.land)
						g_registered.clear();
					return true;
				}
				}
				return true;
			}
			return true;
		}
	}

	void Install()
	{
		if (!SitesAsBuilt(kExecuteLeftOut, std::size(kExecuteLeftOut)) || !SitesAsBuilt(kRevertLeftOut, std::size(kRevertLeftOut)) ||
		    !SitesAsBuilt(kTypesLeftOut, std::size(kTypesLeftOut)))
			return;
		const auto base = pattern::Module::Main().base;
		auto* reg = reinterpret_cast<void*>(base + kRegisterFile);
		g_installed = MH_CreateHook(reg, reinterpret_cast<void*>(&RegisterFileHook), reinterpret_cast<void**>(&g_registerFile)) == MH_OK &&
		              MH_EnableHook(reg) == MH_OK;
		if (!g_installed)
			log::Warn("seamless: the file registration could not be hooked; crossing without a cut is off");
		// (research: the broad phase's long frames told)
		if (auto* add = reinterpret_cast<void*>(base + kBroadPhaseAdd); std::memcmp(add, kBroadPhaseAddBytes, sizeof(kBroadPhaseAddBytes)) == 0)
			if (MH_CreateHook(add, reinterpret_cast<void*>(&BroadPhaseAddHook), reinterpret_cast<void**>(&g_broadPhaseAdd)) != MH_OK || MH_EnableHook(add) != MH_OK)
				log::Warn("seamless: the broad phase could not be watched");
	}

	bool Available()
	{
		return g_installed;
	}

	void SetLands(const std::vector<std::string>& lands)
	{
		g_lands = {lands.begin(), lands.end()};
	}

	std::string Register(const std::string& land, const std::string& there, const float* band)
	{
		if (!g_installed)
			return "not installed (see the log)";
		if (Crossing() || std::any_of(g_jobs.begin(), g_jobs.end(), [&](const Job& j) { return j.land == land; }))
			return "busy (" + Status(true) + ")";
		if (g_stuck.contains(land))
			return "its files were not all given back last time";
		if (!g_ranges.navsEnd)
			g_ranges = FindRanges();
		if (!g_ranges.pathsEnd || !g_ranges.navsEnd)
			return "the road network's or the navmeshes' module not found";
		ReadBase();
		const auto t = Clock::now();
		if (const std::string error = dlcpacks::MountLater(land); !error.empty())
			return error;
		const double mounted = Since(t);
		uint8_t* pack = content::PackRecord(convert::Joaat(land));
		if (!pack)
			return "no pack " + land;
		const auto base = pattern::Module::Main().base;
		reinterpret_cast<ReadChangeSetsFn>(base + kReadChangeSets)(pack);
		Job j{Kind::Register, land, there};
		std::copy(band, band + 4, j.band);
		j.pack = pack;
		std::string error;
		// (its overlay first, then its maps)
		if (!ListSets(pack, kGroupStartup, true, j.sets, error) || !ListSets(pack, kGroupMapSp, true, j.sets, error))
			return error;
		if (j.sets.empty())
			return "no change sets for " + land;
		if (j.sets[0].group != kGroupStartup)
			j.step = 2; // (no overlay)
		landfiles::Prefetch(land, PackOf(land));
		g_overlays[land].clear();
		g_radar[land].clear();
		g_registered.clear();
		g_jobs.push_back(std::move(j));
		Say(std::format("{}: mounted in {:.1f} ms, registration begun ({:.1f} ms)", land, mounted, Since(t)));
		g_frameWork += std::format("begun ({:.1f} ms)", Since(t));
		return "";
	}

	std::string Begin(const std::string& land, const std::string& there, const float* band)
	{
		return Register(land, there, band);
	}

	void Handover(const std::string& to, const std::string& from, const float* band)
	{
		Job j{Kind::Handover, to, from};
		std::copy(band, band + 4, j.band);
		g_jobs.push_back(std::move(j));
		Say(std::format("{} -> {}: the handover queued", from, to));
	}

	void Cancel(const std::string& land, const std::string& there, const float* band)
	{
		Job j{Kind::Cancel, land, there};
		std::copy(band, band + 4, j.band);
		g_jobs.push_back(std::move(j));
		Say(std::format("{}: the registration taken back (queued)", land));
	}

	bool Registered(const std::string& land)
	{
		return g_registered == land && g_jobs.empty();
	}

	bool Busy()
	{
		return !g_jobs.empty();
	}

	void Tick()
	{
		const auto frame = Clock::now();
		if (g_recording)
		{
			std::lock_guard lock(g_noteMutex);
			g_frames.push_back({g_lastFrame, frame, g_frameWork});
			g_frameWork.clear();
		}
		g_lastFrame = frame;
		// (objects waiting for being unloaded, given their files once they are - after 10 s of waiting, those far off and held by
		// nothing unloaded here)
		if (!g_deferred.empty())
		{
			const bool force = Since(g_deferredSince) > 10000;
			natives::ScrVector p{};
			if (force)
				p = natives::Invoke<natives::ScrVector>(0x3FEF770D40960D5A, natives::Invoke<int32_t>(0xD80958FC74E988A6), 1);
			for (auto it = g_deferred.begin(); it != g_deferred.end();)
			{
				if (!SetFile(it->first, it->second) && force && Unload(it->first, p.x, p.y))
					SetFile(it->first, it->second);
				it = Entries()[static_cast<size_t>(it->first) * 2] == it->second ? g_deferred.erase(it) : std::next(it);
			}
		}
		if (g_jobs.empty())
			return;
		Job& j = g_jobs.front();
		const auto t = Clock::now();
		std::string work;
		j.waiting = false;
		const bool done = Step(j, work);
		const double ms = Since(t);
		const char* kinds[] = {"register", "handover", "cancel", "remove"};
		Say(std::format("{} {}: {} in {:.2f} ms", kinds[static_cast<int>(j.kind)], j.land, work, ms));
		Note(std::format("{} ({:.1f} ms)", work, ms));
		if (done)
		{
			log::Info("seamless: {} {} done in {:.0f} ms", kinds[static_cast<int>(j.kind)], j.land, Since(j.began));
			Say(std::format("{} {} done in {:.0f} ms", kinds[static_cast<int>(j.kind)], j.land, Since(j.began)));
			g_jobs.pop_front();
		}
		else if (j.waiting && g_jobs.size() > 1)
		{
			// (waiting: the others go first)
			Job waiting = std::move(g_jobs.front());
			g_jobs.pop_front();
			g_jobs.push_back(std::move(waiting));
		}
	}

	bool Crossing()
	{
		return std::any_of(g_jobs.begin(), g_jobs.end(), [](const Job& j) { return j.kind != Kind::Remove; });
	}

	std::string Status(bool brief)
	{
		std::string out = g_jobs.empty() ? "idle" : std::format("{} job(s), the first at step {}", g_jobs.size(), g_jobs.front().step);
		if (!g_deferred.empty())
			out += std::format("; {} object(s) wait for being unloaded ({:.0f} s)", g_deferred.size(), Since(g_deferredSince) / 1000);
		if (brief)
			return out;
		// (the first of them: a region's or navmesh's square, its flags)
		int listed = 0;
		for (const auto& [object, handle] : g_deferred)
		{
			if (++listed > 40)
				break;
			const bool region = object < g_ranges.pathsEnd;
			const uint32_t n = region ? object - g_ranges.paths : object - g_ranges.navs;
			const auto* origin = reinterpret_cast<const float*>(pattern::Module::Main().base + kNavOrigin);
			const float x = region ? (n % 32) * 1024.0f - 16384 : origin[0] + (n % kNavWidth) * 150.0f;
			const float y = region ? (n / 32) * 1024.0f - 16384 : origin[1] + (n / kNavWidth) * 150.0f;
			out += std::format(" | waits: {} {} at ({:.0f}, {:.0f}), flags {:08x}", region ? "region" : "navmesh", n, x, y,
			    Entries()[static_cast<size_t>(object) * 2 + 1]);
		}
		for (const std::string& s : g_log)
			out += " | " + s;
		return out;
	}

	std::string Profile(uintptr_t from, uintptr_t to)
	{
		// (1 1: the change sets' processing sampled instead, 0 0 nothing)
		g_profilePost = from == 1 && to == 1;
		if (g_profilePost)
			return g_profileFrom = g_profileTo = 0, "the change sets' processing sampled (by their calls in 0xc27740-0xc27e80)";
		g_profileFrom = from, g_profileTo = to;
		return from ? std::format("the data files sampled by their calls in {:#x}-{:#x}", from, to) : "not sampled";
	}

	std::string Frames(bool start, const std::vector<unsigned long>& others)
	{
		if (start)
		{
			g_frames.clear();
			g_recording = true;
			// (thread 0: none sampled - the frames' times alone, without the sampler stopping threads a thousand times a
			// second)
			if (!others.empty() && others[0] == 0)
				return "recording (no thread sampled)";
			debug::profile::Start(others);
			return std::format("recording (the game's thread {} and {} more sampled)", GetCurrentThreadId(), others.size());
		}
		g_recording = false;
		debug::profile::Stop();
		if (g_frames.empty())
			return "no frames";
		std::vector<double> times;
		for (const Frame& f : g_frames)
			times.push_back(f.ms());
		std::sort(times.begin(), times.end());
		const auto at = [&](double q) { return times[std::min(times.size() - 1, static_cast<size_t>(q * times.size()))]; };
		size_t over = 0;
		std::string slow;
		for (const Frame& f : g_frames)
			if (f.ms() > 15 + at(0.5))
			{
				++over;
				// (when: seconds into the recording, and the work of the frames just before)
				const size_t k = static_cast<size_t>(&f - g_frames.data());
				std::string before;
				for (size_t b = k > 3 ? k - 3 : 0; b < k; ++b)
					if (!g_frames[b].work.empty())
						before += std::format("{}{}", before.empty() ? "" : "; ", g_frames[b].work.substr(0, 80));
				if (slow.size() < 40000)
					slow += std::format(" | {:.1f} ms at {:.2f} s: {} [{}]{}", f.ms(), std::chrono::duration<double>(f.start - g_frames.front().start).count(),
					    f.work.empty() ? "-" : f.work, debug::profile::Between(f.start, f.end), before.empty() ? "" : " (before: " + before + ")");
			}
		// (research: the broad phase's long takes all along, long frame or not)
		std::string broad;
		size_t takes = 0;
		for (const Frame& f : g_frames)
			for (size_t k = f.work.find("broad phase took "); k != std::string::npos; k = f.work.find("broad phase took ", k + 1))
				if (++takes <= 40)
					broad += std::format(" {:.2f} s {:.1f} ms {}", std::chrono::duration<double>(f.start - g_frames.front().start).count(), f.ms(),
					    f.work.substr(k + 17, f.work.find(';', k) - k - 17));
		return std::format("{} frames: median {:.1f} ms, p99 {:.1f}, longest {:.1f}; {} more than 15 ms over the median{}{}", g_frames.size(), at(0.5),
		    at(0.99), times.back(), over, slow, takes ? std::format(" | the broad phase's takes of 3 ms and more: {}:{}", takes, broad) : "");
	}

	void Note(const std::string& what)
	{
		std::lock_guard lock(g_noteMutex);
		if (g_recording)
			g_frameWork += (g_frameWork.empty() ? "" : "; ") + what;
	}
}
