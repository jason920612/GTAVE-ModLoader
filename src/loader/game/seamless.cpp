#include "seamless.hpp"

#include <Windows.h>

#include <algorithm>
#include <array>
#include <charconv>
#include <chrono>
#include <cstring>
#include <format>
#include <map>
#include <string>
#include <string_view>
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
		constexpr uintptr_t kExecuteSet = 0xC272A0;
		using ExecuteSetFn = void (*)(void* manager, void* pack, const uint32_t* group, const uint32_t* set, uint32_t flags);
		struct Site
		{
			uintptr_t at;
			uint8_t size;
			uintptr_t target; // (a call rel32: where it goes)
			uint8_t bytes[3]; // (a call through a vtable: its bytes)
		};
		constexpr Site kLeftOut[] = {{0xC27B8F, 5, 0x3A6370}, {0xC27B94, 5, 0x127A3A0}, {0xC27BA0, 5, 0xCEECD0}, {0xC27BB4, 5, 0x926EE0},
		    {0xC27BBB, 5, 0x112D1F0}, {0xC27BC5, 5, 0x1D94BB0}, {0xC27C45, 5, 0xC527D0}, {0xC27C63, 5, 0x926EE0}, {0xC27C6B, 5, 0xC2E0B0},
		    {0xC27D48, 5, 0xC2E0B0}, {0xC27E44, 5, 0xCF03F0}, {0xC27DAD, 3, 0, {0xFF, 0x50, 0x30}}, {0xC27DBE, 3, 0, {0xFF, 0x50, 0x10}},
		    // (the map change data's 0xC0F420: at its end the streaming objects of the change set's files looked for and marked for
		    // loading again - 0xCEA720 twice, through all the objects for each archive: 190 ms for a land's first set; new files,
		    // none loaded)
		    {0xC0FD23, 5, 0xCEA720}, {0xC0FD30, 5, 0xCEA720}};
		// the box trees: the map store's box streamer (+0x3ED91D0, vtable +0x30) and the collision store's (+0x3ED8E30, +0x10)
		constexpr uintptr_t kMapStreamer = 0x3ED91D0, kBoundsStreamer = 0x3ED8E30;

		// The streaming objects (+0x42F9E48: +0 8 bytes each, the handle - archive << 16 | its entry - first) of the road
		// network's regions (the Paths module, its first object + region = y * 32 + x on 1024 m from -16384; past 1024 the
		// island's set) and of the navmeshes (NavMeshes, + y * 147 + x on 150 m squares from the runtime origin, +0x42F6580;
		// a file "navmesh[<x * 3>][<y * 3>]", research/phase0.md §61.8): each one file, its handle. A land's overlay (its
		// start-up change set) brings both. Its files are registered one by one (0xCE3320: rcx the streaming, rdx where the
		// object's index goes - -1 refused -, r8 the file's name, r9d its handle; nine arguments); with the pack flag set as
		// the map path sets it, one for a name the land there has takes it over (0xCE36C5), and for a loaded object it first
		// looks through all the objects for those depending on it (0xCEA4B0 with 2: ~5 ms each) - the region and the
		// navmeshes round the player. So outside the band they are refused here, as the game refuses (their handles kept
		// for the crossing); in the band they take over.
		constexpr uintptr_t kStreaming = 0x42F9E48, kNavOrigin = 0x42F6580, kRegisterFile = 0xCE3320;
		constexpr uintptr_t kPathsVtable = 0x2646520, kNavMeshesVtable = 0x262DBA0;
		constexpr uint32_t kNavWidth = 147;
		using RegisterFileFn = uint64_t (*)(void*, uint32_t*, const char*, uint32_t, uint64_t, uint64_t, uint64_t, uint64_t, uint64_t);
		RegisterFileFn g_registerFile = nullptr;
		std::vector<std::pair<std::string, uint32_t>>* g_capture = nullptr;
		DWORD g_captureThread = 0;
		bool Admitted(std::string_view name);

		uint64_t RegisterFileHook(void* a, uint32_t* index, const char* name, uint32_t handle, uint64_t e, uint64_t f, uint64_t g, uint64_t h, uint64_t i)
		{
			if (g_capture && name && GetCurrentThreadId() == g_captureThread)
			{
				g_capture->push_back({name, handle});
				if (!Admitted(name))
				{
					*index = 0xFFFFFFFF;
					return reinterpret_cast<uint64_t>(index);
				}
			}
			return g_registerFile(a, index, name, handle, e, f, g, h, i);
		}

		bool g_installed = false; // (the sites as the build has them, the registration hooked)

		// the left-out calls made nothing (on) or put back
		void LeaveOut(bool on)
		{
			static std::array<std::array<uint8_t, 5>, std::size(kLeftOut)> saved{};
			static constexpr uint8_t kNop5[] = {0x0F, 0x1F, 0x44, 0x00, 0x00}, kNop3[] = {0x0F, 0x1F, 0x00};
			const auto base = pattern::Module::Main().base;
			for (size_t i = 0; i < std::size(kLeftOut); ++i)
			{
				auto* at = reinterpret_cast<uint8_t*>(base + kLeftOut[i].at);
				DWORD old = 0;
				VirtualProtect(at, kLeftOut[i].size, PAGE_EXECUTE_READWRITE, &old);
				if (on)
				{
					std::memcpy(saved[i].data(), at, kLeftOut[i].size);
					std::memcpy(at, kLeftOut[i].size == 5 ? kNop5 : kNop3, kLeftOut[i].size);
				}
				else
					std::memcpy(at, saved[i].data(), kLeftOut[i].size);
				VirtualProtect(at, kLeftOut[i].size, old, &old);
				FlushInstructionCache(GetCurrentProcess(), at, kLeftOut[i].size);
			}
		}

		// the Paths' and NavMeshes' objects: their first, their end
		struct Ranges
		{
			uint32_t paths = 0, pathsEnd = 0, navs = 0, navsEnd = 0;
		};
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
					r.paths = first, r.pathsEnd = end;
				else if (vt == kNavMeshesVtable)
					r.navs = first, r.navsEnd = end;
			}
			return r;
		}

		// a road network region's or navmesh's object from its file's name ("nodes<n>", "navmesh[<x>][<y>]", a path before it
		// or not), or 0
		uint32_t ObjectOf(const Ranges& r, std::string_view name)
		{
			if (const size_t slash = name.find_last_of("/\\"); slash != std::string_view::npos)
				name.remove_prefix(slash + 1);
			const auto number = [&](size_t at, uint32_t& n) {
				const auto [end, ec] = std::from_chars(name.data() + at, name.data() + name.size(), n);
				return ec == std::errc() ? static_cast<size_t>(end - name.data()) : std::string_view::npos;
			};
			uint32_t n = 0, x = 0, y = 0;
			if (name.starts_with("nodes") && number(5, n) != std::string_view::npos)
				return n < 1024 ? r.paths + n : 0;
			if (name.starts_with("navmesh["))
			{
				const size_t at = number(8, x);
				if (at != std::string_view::npos && name.substr(at).starts_with("][") && number(at + 2, y) != std::string_view::npos)
					return r.navs + (y / 3) * kNavWidth + x / 3;
			}
			return 0;
		}

		enum class Phase { Idle, Files, Post, Handles, LandOff, BandOn, ThereOff, BoundsTree, MapTree, Done };
		struct ChangeSet
		{
			uint32_t group = 0, hash = 0;
			std::vector<std::array<uint8_t, 24>> entries;
		};
		struct Registration
		{
			std::string land, there;
			float band[4]{};
			uint8_t* pack = nullptr;
			std::vector<ChangeSet> sets;
			size_t set = 0, entry = 0;
			Phase phase = Phase::Idle;
			Clock::time_point began;
			Ranges ranges;
			std::vector<uint32_t> before; // (the Paths' and NavMeshes' handles before the land's overlay)
			std::vector<std::pair<std::string, uint32_t>> captured;
			std::vector<std::string> steps; // (each frame's work and its time)
		} g_reg;

		// whether a region's or navmesh's object is in the band of the land being registered (its square overlapping it)
		bool InBand(uint32_t object)
		{
			const Ranges& r = g_reg.ranges;
			const float* band = g_reg.band;
			const auto overlaps = [&](float x0, float y0, float size) { return x0 + size > band[0] && x0 < band[2] && y0 + size > band[1] && y0 < band[3]; };
			if (object < r.pathsEnd)
			{
				const uint32_t n = object - r.paths;
				return n < 1024 && overlaps((n % 32) * 1024.0f - 16384, (n / 32) * 1024.0f - 16384, 1024);
			}
			const uint32_t n = object - r.navs;
			const auto* origin = reinterpret_cast<const float*>(pattern::Module::Main().base + kNavOrigin);
			return overlaps(origin[0] + (n % kNavWidth) * 150.0f, origin[1] + (n / kNavWidth) * 150.0f, 150);
		}

		// a file of the land's overlay to be registered: all but a region or a navmesh outside the band
		bool Admitted(std::string_view name)
		{
			const uint32_t object = ObjectOf(g_reg.ranges, name);
			return !object || InBand(object);
		}

		constexpr double kBudget = 2.5; // (ms of data files a frame, at least one)
		bool g_profilePost = false;     // (research: the change sets' processing sampled)
		uintptr_t g_profileFrom = 0, g_profileTo = 0; // (research: the data files' enabling sampled, by its calls in this range)

		// a data file entry's file name
		std::string NameOf(const uint8_t* entry)
		{
			const auto* file = *reinterpret_cast<const char* const*>(entry);
			char name[96] = "?";
			if (file)
				strncpy_s(name, file, _TRUNCATE);
			return name;
		}

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
	}

	void Install()
	{
		const auto base = pattern::Module::Main().base;
		for (const Site& s : kLeftOut)
		{
			const auto* at = reinterpret_cast<const uint8_t*>(base + s.at);
			int32_t rel = 0;
			std::memcpy(&rel, at + 1, 4);
			if (s.size == 5 ? at[0] != 0xE8 || s.at + 5 + rel != s.target : std::memcmp(at, s.bytes, 3) != 0)
			{
				log::Warn("seamless: the map path differs from the build this was made for (at {:#x}); crossing without a cut is off", s.at);
				return;
			}
		}
		auto* reg = reinterpret_cast<void*>(base + kRegisterFile);
		g_installed = MH_CreateHook(reg, reinterpret_cast<void*>(&RegisterFileHook), reinterpret_cast<void**>(&g_registerFile)) == MH_OK &&
		              MH_EnableHook(reg) == MH_OK;
		if (!g_installed)
			log::Warn("seamless: the file registration could not be hooked; crossing without a cut is off");
	}

	std::string Begin(const std::string& land, const std::string& there, const float* band)
	{
		if (g_reg.phase != Phase::Idle && g_reg.phase != Phase::Done)
			return "a registration is under way (" + g_reg.land + ")";
		if (!g_installed)
			return "not installed (see the log)";
		const Ranges ranges = FindRanges();
		if (!ranges.pathsEnd || !ranges.navsEnd)
			return "the road network's or the navmeshes' module not found";
		const auto t = Clock::now();
		if (const std::string error = dlcpacks::MountLater(land); !error.empty())
			return error;
		const double mounted = Since(t);
		uint8_t* pack = content::PackRecord(convert::Joaat(land));
		if (!pack)
			return "no pack " + land;
		const auto base = pattern::Module::Main().base;
		const auto t1 = Clock::now();
		reinterpret_cast<ReadChangeSetsFn>(base + kReadChangeSets)(pack);
		const double read = Since(t1);
		const auto list = reinterpret_cast<ListFn>(base + kListDataFiles);
		const auto freeList = reinterpret_cast<FreeFn>(base + kFree);
		g_reg = {};
		g_reg.land = land;
		g_reg.there = there;
		std::copy(band, band + 4, g_reg.band);
		g_reg.pack = pack;
		g_reg.ranges = ranges;
		const auto* entries = *reinterpret_cast<const uint32_t* const*>(base + kStreaming);
		for (uint32_t i = ranges.paths; i < ranges.navsEnd; ++i)
			g_reg.before.push_back(entries[static_cast<size_t>(i) * 2]);
		const auto t2 = Clock::now();
		// (its overlay first, then its maps)
		for (const uint32_t group : {kGroupStartup, kGroupMapSp})
		{
			uint16_t count = 0;
			const uint32_t* hashes = content::GroupChangeSets(pack, group, count);
			for (uint16_t i = 0; hashes && i < count; ++i)
			{
				uint8_t* record = content::ChangeSetRecord(hashes[i]);
				if (!record)
					return std::format("no record of change set {:08x}", hashes[i]);
				List l;
				list(record + 8, &l, -1, 0, 1);
				ChangeSet cs{group, hashes[i]};
				for (uint16_t e = 0; e < l.count; ++e)
				{
					std::array<uint8_t, 24> entry{};
					std::memcpy(entry.data(), l.entries + static_cast<size_t>(e) * 24, 24);
					cs.entries.push_back(entry);
				}
				if (l.capacity && l.entries)
					freeList(l.entries);
				g_reg.sets.push_back(std::move(cs));
			}
		}
		if (g_reg.sets.empty())
			return "no change sets for " + land;
		g_reg.phase = Phase::Files;
		g_reg.began = Clock::now();
		g_reg.steps.push_back(std::format("mounted in {:.1f} ms, its change sets read in {:.1f} ms, {} listed in {:.1f} ms (all {:.1f} ms)", mounted, read,
		    g_reg.sets.size(), Since(t2), Since(t)));
		g_frameWork += std::format("begun ({:.1f} ms)", Since(t));
		return "";
	}

	std::string Profile(uintptr_t from, uintptr_t to)
	{
		g_profileFrom = from, g_profileTo = to;
		return from ? std::format("the data files sampled by their calls in {:#x}-{:#x}", from, to) : "not sampled";
	}

	void Tick()
	{
		const auto frame = Clock::now();
		if (g_recording)
		{
			g_frames.push_back({g_lastFrame, frame, g_frameWork});
			g_frameWork.clear();
		}
		g_lastFrame = frame;
		if (g_reg.phase == Phase::Idle || g_reg.phase == Phase::Done)
			return;
		const auto base = pattern::Module::Main().base;
		const auto t = Clock::now();
		std::string work;
		switch (g_reg.phase)
		{
		case Phase::Files:
		{
			const auto enable = reinterpret_cast<EnableFn>(base + kEnableDataFile);
			const auto setPackFlag = reinterpret_cast<SetPackFlagFn>(base + kSetPackFlag);
			ChangeSet& cs = g_reg.sets[g_reg.set];
			int done = 0;
			std::string slow;
			while (g_reg.entry < cs.entries.size() && (done == 0 || Since(t) < kBudget))
			{
				auto& entry = cs.entries[g_reg.entry++];
				if (*reinterpret_cast<const uint32_t*>(entry.data() + 8) == 4)
					continue;
				g_capture = cs.group == kGroupStartup ? &g_reg.captured : nullptr;
				g_captureThread = GetCurrentThreadId();
				const size_t seen = g_reg.captured.size();
				const auto t1 = Clock::now();
				setPackFlag(*reinterpret_cast<const uint32_t*>(g_reg.pack + 0xFC));
				std::string samples;
				if (g_profileFrom)
					samples = " [" + debug::profile::Profiled(g_profileFrom, g_profileTo, [&] { enable(entry.data()); }) + "]";
				else
					enable(entry.data());
				setPackFlag(0);
				g_capture = nullptr;
				if (const double ms = Since(t1); ms > 5)
					slow += std::format("; {} kind {} ({} files) {:.1f} ms{}", NameOf(entry.data()), *reinterpret_cast<const uint32_t*>(entry.data() + 8),
					    g_reg.captured.size() - seen, ms, samples);
				++done;
			}
			work = std::format("{} data file(s) of set {}{}", done, g_reg.set, slow);
			if (g_reg.entry >= cs.entries.size())
				g_reg.phase = Phase::Post;
			break;
		}
		case Phase::Post:
		{
			const ChangeSet& cs = g_reg.sets[g_reg.set];
			const uint32_t group = cs.group, set = cs.hash;
			LeaveOut(true);
			const auto run = [&] { reinterpret_cast<ExecuteSetFn>(base + kExecuteSet)(content::ManagerRecord(), g_reg.pack, &group, &set, 1); };
			std::string samples;
			if (g_profilePost)
				samples = " [" + debug::profile::Profiled(0xC27740, 0xC27E80, run) + "]";
			else
				run();
			LeaveOut(false);
			work = std::format("set {} ({:08x}) processed{}", g_reg.set, set, samples);
			const bool overlayDone = cs.group == kGroupStartup && (g_reg.set + 1 >= g_reg.sets.size() || g_reg.sets[g_reg.set + 1].group != kGroupStartup);
			if (++g_reg.set < g_reg.sets.size())
				g_reg.entry = 0, g_reg.phase = Phase::Files;
			else
				g_reg.phase = Phase::LandOff;
			if (overlayDone)
				g_reg.phase = Phase::Handles; // (then on with the maps)
			break;
		}
		case Phase::Handles:
		{
			// the band's regions and navmeshes the land has none for: the land there's taken out (its files took the others
			// over as they were registered, those outside were refused)
			auto* entries = *reinterpret_cast<uint32_t**>(base + kStreaming);
			const Ranges& r = g_reg.ranges;
			std::map<uint32_t, uint32_t> theirs; // (object -> the land's handle)
			size_t regions = 0, navmeshes = 0, inBand = 0, mismatched = 0, emptied = 0, outsideChanged = 0, loaded = 0;
			for (const auto& [name, handle] : g_reg.captured)
				if (const uint32_t o = ObjectOf(r, name))
					theirs[o] = handle, ++(o < r.pathsEnd ? regions : navmeshes);
			for (uint32_t i = r.paths; i < r.navsEnd; ++i)
			{
				if (i >= r.paths + 1024 && i < r.pathsEnd)
					continue; // (the island's set)
				uint32_t& handle = entries[static_cast<size_t>(i) * 2];
				if (!InBand(i))
				{
					outsideChanged += handle != g_reg.before[i - r.paths];
					continue;
				}
				++inBand;
				const auto it = theirs.find(i);
				if (it != theirs.end())
					mismatched += handle != it->second;
				else if (handle)
				{
					loaded += (entries[static_cast<size_t>(i) * 2 + 1] & 3) != 0;
					handle = 0, ++emptied;
				}
			}
			work = std::format("{} files of {}'s overlay seen ({} regions, {} navmeshes); in the band {} objects: {} not its file, {} emptied ({} "
			                   "loaded); outside {} changed",
			    g_reg.captured.size(), g_reg.land, regions, navmeshes, inBand, mismatched, emptied, loaded, outsideChanged);
			g_reg.phase = g_reg.set < g_reg.sets.size() ? Phase::Files : Phase::LandOff;
			break;
		}
		// each land's part: the new one only in the band, the one there no more in it (a frame each)
		case Phase::LandOff:
		case Phase::BandOn:
		case Phase::ThereOff:
		{
			static constexpr float kAll[4] = {-16000, -16000, 16000, 16000};
			const auto pack = [](const std::string& l) { return paths::Get().mods / l / L"dlc.rpf"; };
			size_t n = 0;
			if (g_reg.phase == Phase::LandOff)
				n = landfiles::SwitchArea(g_reg.land, pack(g_reg.land), kAll, false), work = std::format("{} of {} off", n, g_reg.land);
			else if (g_reg.phase == Phase::BandOn)
				n = landfiles::SwitchArea(g_reg.land, pack(g_reg.land), g_reg.band, true), work = std::format("{} of {} on in the band", n, g_reg.land);
			else if (!g_reg.there.empty())
				n = landfiles::SwitchArea(g_reg.there, pack(g_reg.there), g_reg.band, false), work = std::format("{} of {} off in the band", n, g_reg.there);
			g_reg.phase = static_cast<Phase>(static_cast<int>(g_reg.phase) + 1);
			break;
		}
		case Phase::BoundsTree:
		{
			auto* streamer = reinterpret_cast<uint8_t*>(base + kBoundsStreamer);
			(*reinterpret_cast<void (**)(uint8_t*)>(*reinterpret_cast<uintptr_t*>(streamer) + 0x10))(streamer);
			work = "the collision box tree made again";
			g_reg.phase = Phase::MapTree;
			break;
		}
		case Phase::MapTree:
		{
			auto* streamer = reinterpret_cast<uint8_t*>(base + kMapStreamer);
			(*reinterpret_cast<void (**)(uint8_t*)>(*reinterpret_cast<uintptr_t*>(streamer) + 0x30))(streamer);
			work = "the map box tree made again";
			g_reg.phase = Phase::Done;
			break;
		}
		default:
			break;
		}
		const double ms = Since(t);
		g_reg.steps.push_back(std::format("{} in {:.2f} ms", work, ms));
		g_frameWork = std::format("{} ({:.1f} ms)", work, ms);
		if (g_reg.phase == Phase::Done)
		{
			g_reg.steps.push_back(std::format("{} registered over {:.0f} ms", g_reg.land, Since(g_reg.began)));
			log::Info("seamless: {} registered over {} frames ({:.0f} ms)", g_reg.land, g_reg.steps.size() - 2, Since(g_reg.began));
		}
	}

	std::string Status()
	{
		std::string out = std::format("{} phase {}:", g_reg.land, static_cast<int>(g_reg.phase));
		for (const std::string& s : g_reg.steps)
			out += " | " + s;
		return out;
	}

	std::string Frames(bool start, const std::vector<unsigned long>& others)
	{
		if (start)
		{
			g_frames.clear();
			g_recording = true;
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
				if (slow.size() < 4000)
					slow += std::format(" | {:.1f} ms: {} [{}]", f.ms(), f.work.empty() ? "-" : f.work, debug::profile::Between(f.start, f.end));
			}
		return std::format("{} frames: median {:.1f} ms, p99 {:.1f}, longest {:.1f}; {} more than 15 ms over the median{}", g_frames.size(), at(0.5),
		    at(0.99), times.back(), over, slow);
	}
}
