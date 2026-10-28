#include "seamless.hpp"

#include <Windows.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <cstring>
#include <format>
#include <map>
#include <string>
#include <vector>

#include "../convert/resource.hpp"
#include "../debug/profile.hpp"
#include "../log.hpp"
#include "../pattern.hpp"
#include "content.hpp"
#include "dlcpacks.hpp"
#include "landfiles.hpp"
#include "../paths.hpp"

namespace loader::game::seamless
{
	namespace
	{
		using Clock = std::chrono::steady_clock;
		double Since(Clock::time_point t)
		{
			return std::chrono::duration<double, std::milli>(Clock::now() - t).count();
		}

		constexpr uint32_t kGroupMapSp = 0x578F99E2;
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
		// the map path for one change set (0xC27740), here with flags 1 and these of its calls left out: the waits for the
		// other threads (0x127A3A0, 0xCEECD0, 0x926EE0 twice), the world's resets (0x3A6370, 0x112D1F0, 0x1D94BB0 - each
		// ped's navmesh tracker -, 0xC527D0), its data files (done already, 0xC2E0B0), "everything asked for loaded now"
		// (0xCF03F0) and the two box trees made again (vtable calls: done in frames of their own).
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
		    {0xC27E44, 5, 0xCF03F0}, {0xC27DAD, 3, 0, {0xFF, 0x50, 0x30}}, {0xC27DBE, 3, 0, {0xFF, 0x50, 0x10}},
		    // (the map change data's 0xC0F420: at its end the streaming objects of the change set's files looked for and marked for
		    // loading again - 0xCEA720 twice, through all the objects for each archive: 190 ms for a land's first set; new files,
		    // none loaded)
		    {0xC0FD23, 5, 0xCEA720}, {0xC0FD30, 5, 0xCEA720}};
		// the box trees: the map store's box streamer (+0x3ED91D0, vtable +0x30) and the collision store's (+0x3ED8E30, +0x10)
		constexpr uintptr_t kMapStreamer = 0x3ED91D0, kBoundsStreamer = 0x3ED8E30;

		// The sites as the build has them (checked once)
		bool SitesKnown()
		{
			static const bool known = [] {
				const auto base = pattern::Module::Main().base;
				for (const Site& s : kLeftOut)
				{
					const auto* at = reinterpret_cast<const uint8_t*>(base + s.at);
					if (s.size == 5)
					{
						int32_t rel = 0;
						std::memcpy(&rel, at + 1, 4);
						if (at[0] != 0xE8 || s.at + 5 + rel != s.target)
							return false;
					}
					else if (std::memcmp(at, s.bytes, 3) != 0)
						return false;
				}
				return true;
			}();
			return known;
		}

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

		enum class Phase { Idle, Files, Post, LandOff, BandOn, ThereOff, BoundsTree, MapTree, Done };
		struct ChangeSet
		{
			uint32_t hash = 0;
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
			std::vector<std::string> steps; // (each frame's work and its time)
		} g_reg;
		constexpr double kBudget = 2.5; // (ms of data files a frame, at least one)
		bool g_profilePost = true;      // (research: the change sets' processing sampled)

		// the frames' times while recording (ms), and the steps done in each
		bool g_recording = false;
		Clock::time_point g_lastFrame;
		std::vector<std::pair<double, std::string>> g_frames;
		std::string g_frameWork;
	}

	std::string Begin(const std::string& land, const std::string& there, const float* band)
	{
		if (g_reg.phase != Phase::Idle && g_reg.phase != Phase::Done)
			return "a registration is under way (" + g_reg.land + ")";
		if (!SitesKnown())
			return "the map path differs from the build this was made for";
		const auto t = Clock::now();
		if (const std::string error = dlcpacks::MountLater(land); !error.empty())
			return error;
		const double mounted = Since(t);
		uint8_t* pack = content::PackRecord(convert::Joaat(land));
		uint16_t count = 0;
		const uint32_t* hashes = pack ? content::GroupChangeSets(pack, kGroupMapSp, count) : nullptr;
		if (!hashes)
			return "no map change sets for " + land;
		const auto base = pattern::Module::Main().base;
		reinterpret_cast<ReadChangeSetsFn>(base + kReadChangeSets)(pack);
		const auto list = reinterpret_cast<ListFn>(base + kListDataFiles);
		const auto freeList = reinterpret_cast<FreeFn>(base + kFree);
		g_reg = {};
		g_reg.land = land;
		g_reg.there = there;
		std::copy(band, band + 4, g_reg.band);
		g_reg.pack = pack;
		const auto t2 = Clock::now();
		for (uint16_t i = 0; i < count; ++i)
		{
			uint8_t* record = content::ChangeSetRecord(hashes[i]);
			if (!record)
				return std::format("no record of change set {:08x}", hashes[i]);
			List l;
			list(record + 8, &l, -1, 0, 1);
			ChangeSet cs{hashes[i]};
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
		g_reg.phase = Phase::Files;
		g_reg.began = Clock::now();
		g_reg.steps.push_back(std::format("mounted in {:.1f} ms, {} change set(s) listed in {:.1f} ms", mounted, count, Since(t2)));
		return "";
	}

	void Tick()
	{
		const auto frame = Clock::now();
		if (g_recording)
		{
			g_frames.push_back({std::chrono::duration<double, std::milli>(frame - g_lastFrame).count(), g_frameWork});
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
			while (g_reg.entry < cs.entries.size() && (done == 0 || Since(t) < kBudget))
			{
				auto& entry = cs.entries[g_reg.entry++];
				if (*reinterpret_cast<const uint32_t*>(entry.data() + 8) == 4)
					continue;
				setPackFlag(*reinterpret_cast<const uint32_t*>(g_reg.pack + 0xFC));
				enable(entry.data());
				setPackFlag(0);
				++done;
			}
			work = std::format("{} data file(s) of set {}", done, g_reg.set);
			if (g_reg.entry >= cs.entries.size())
				g_reg.phase = Phase::Post;
			break;
		}
		case Phase::Post:
		{
			const uint32_t group = kGroupMapSp, set = g_reg.sets[g_reg.set].hash;
			LeaveOut(true);
			const auto run = [&] { reinterpret_cast<ExecuteSetFn>(base + kExecuteSet)(content::ManagerRecord(), g_reg.pack, &group, &set, 1); };
			std::string samples;
			if (g_profilePost)
				samples = " [" + debug::profile::Profiled(0xC27740, 0xC27E80, run) + "]";
			else
				run();
			LeaveOut(false);
			work = std::format("set {} ({:08x}) processed{}", g_reg.set, set, samples);
			if (++g_reg.set < g_reg.sets.size())
				g_reg.entry = 0, g_reg.phase = Phase::Files;
			else
				g_reg.phase = Phase::LandOff;
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

	std::string Frames(bool start)
	{
		if (start)
		{
			g_frames.clear();
			g_recording = true;
			return "recording";
		}
		g_recording = false;
		if (g_frames.empty())
			return "no frames";
		std::vector<double> times;
		for (const auto& f : g_frames)
			times.push_back(f.first);
		std::sort(times.begin(), times.end());
		const auto at = [&](double q) { return times[std::min(times.size() - 1, static_cast<size_t>(q * times.size()))]; };
		size_t over = 0;
		std::string slow;
		for (const auto& [ms, w] : g_frames)
			if (ms > 15 + at(0.5))
			{
				++over;
				if (slow.size() < 1500)
					slow += std::format(" | {:.1f} ms: {}", ms, w.empty() ? "-" : w);
			}
		return std::format("{} frames: median {:.1f} ms, p99 {:.1f}, longest {:.1f}; {} more than 15 ms over the median{}", g_frames.size(), at(0.5),
		    at(0.99), times.back(), over, slow);
	}
}
