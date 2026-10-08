#include "mission.hpp"

#include <algorithm>
#include <climits>
#include <cstring>
#include <map>
#include <optional>
#include <set>
#include <string>
#include <vector>

#include <modloader/modloader.hpp>
#include <modloader/natives.hpp>
#include <modloader/script.hpp>

namespace mission
{
	namespace
	{
		namespace sc = ml::script;

		// Script globals of build 0x6aa45f10 (research/phase0.md §22).
		constexpr uint32_t kFlowPassed = 65082;    // written by the story missions' shared "passed" helper
		const ml::Global kStatsPassed{65074};      // mission_stat_watcher shows the results once it is set
		const ml::Global kStatCount{77175};        // stats the current mission tracks
		const ml::Global kStatList{77176};         // array: entries of 9 slots (stat id, value, ..., state at +3)
		const ml::Global kStatDefs{65305};         // array: definitions of 13 slots (type, ?, target, lower is better,
		                                           // ..., hidden at +7)
		constexpr uint64_t kTerminateThisThread = 0x1090044AD1DA76FA;
		constexpr int kStatTypeWatcherValue = 15; // value computed by the results screen itself

		struct Function
		{
			uint32_t start, end, params;
		};

		struct Entry
		{
			bool inlineInMain = false; // continue inside main (the pass is one of its cases) instead of a call
			uint32_t address = 0;
			uint32_t params = 0;
			// Missions run as a stage machine (main switches on a static each frame, and the pass routine is one stage
			// that runs over several frames): the stage static is set instead, so the mission's loop runs it.
			bool stage = false;
			uint32_t stageStatic = 0;
			int32_t stageValue = 0;
			// The pass routine is a stage machine called by main every frame once main's own check says the mission is
			// done: main is sent to that call every frame (`address`, inside main) until the script ends.
			bool repeatInMain = false;
		};

		struct Program
		{
			std::vector<uint8_t> code;
			std::vector<sc::Instruction> instructions;
			std::vector<Function> functions;

			size_t FunctionAt(uint32_t address) const
			{
				const auto it = std::upper_bound(functions.begin(), functions.end(), address, [](uint32_t a, const Function& f) { return a < f.start; });
				return static_cast<size_t>(it - functions.begin()) - 1;
			}
		};

		bool Load(int32_t id, Program& p)
		{
			p.code = ml::scripts::Code(id);
			for (uint32_t at = 0; at < p.code.size();)
			{
				const auto in = sc::Decode(p.code, at);
				if (!in.length)
					return false;
				if (in.op == sc::ENTER)
				{
					if (!p.functions.empty())
						p.functions.back().end = at;
					p.functions.push_back({at, 0, static_cast<uint32_t>(in.operand)});
				}
				p.instructions.push_back(in);
				at += in.length;
			}
			if (p.functions.empty() || p.functions.front().start != 0)
				return false;
			p.functions.back().end = static_cast<uint32_t>(p.code.size());
			return true;
		}

		// The first instruction of the straight-line block that contains `address` inside function `f`.
		uint32_t BlockStart(const Program& p, const Function& f, uint32_t address)
		{
			std::set<uint32_t> leaders{f.start};
			for (const auto& in : p.instructions)
			{
				if (in.address < f.start || in.address >= f.end)
					continue;
				if (sc::IsJump(in.op))
					leaders.insert(static_cast<uint32_t>(in.operand));
				if (in.op == sc::SWITCH)
					for (uint32_t k = 0; k < in.operand; ++k)
						leaders.insert(sc::SwitchTarget(p.code, in.address, k));
				if (in.op == sc::J || in.op == sc::SWITCH || in.op == sc::LEAVE || sc::IsJump(in.op))
					leaders.insert(in.address + in.length);
			}
			return *std::prev(leaders.upper_bound(address));
		}

		// The function switches on a static near its start: it runs one step per call.
		bool IsStageMachine(const Program& p, size_t f)
		{
			const auto& fn = p.functions[f];
			size_t k = static_cast<size_t>(std::lower_bound(p.instructions.begin(), p.instructions.end(), fn.start,
			                                   [](const sc::Instruction& in, uint32_t a) { return in.address < a; }) -
			                               p.instructions.begin());
			for (int step = 0; step < 8 && k + 1 < p.instructions.size() && p.instructions[k + 1].address < fn.end; ++step, ++k)
			{
				const uint8_t op = p.instructions[k].op;
				if ((op == 59 || op == 80 || op == 95) && p.instructions[k + 1].op == sc::SWITCH)
					return true;
			}
			return false;
		}

		// A SWITCH on a static whose case starts by calling the pass routine `f` (or a small function that does): the
		// mission's stage machine. Returns the static and the case value.
		std::optional<Entry> FindStage(const Program& p, size_t f, const std::vector<std::vector<std::pair<size_t, uint32_t>>>& callers)
		{
			std::set<uint32_t> targets{p.functions[f].start};
			for (const auto& [caller, site] : callers[f])
				if (caller != 0 && p.functions[caller].end - p.functions[caller].start < 64)
					targets.insert(p.functions[caller].start);
			const auto index = [&](uint32_t address) {
				return static_cast<size_t>(std::lower_bound(p.instructions.begin(), p.instructions.end(), address,
				                               [](const sc::Instruction& in, uint32_t a) { return in.address < a; }) -
				                           p.instructions.begin());
			};
			for (size_t k = 1; k < p.instructions.size(); ++k)
			{
				const auto& sw = p.instructions[k];
				const auto& load = p.instructions[k - 1];
				if (sw.op != sc::SWITCH || (load.op != 59 && load.op != 80 && load.op != 95)) // STATIC_U8/U16/U24_LOAD
					continue;
				for (uint32_t c = 0; c < sw.operand; ++c)
				{
					int32_t value;
					std::memcpy(&value, p.code.data() + sw.address + 2 + 6 * c, 4);
					size_t t = index(sc::SwitchTarget(p.code, sw.address, c));
					for (int step = 0; step < 6 && t < p.instructions.size(); ++step, ++t)
					{
						const auto& in = p.instructions[t];
						if (in.op == sc::CALL && targets.contains(static_cast<uint32_t>(in.operand)))
						{
							Entry e;
							e.stage = true;
							e.stageStatic = static_cast<uint32_t>(load.operand);
							e.stageValue = value;
							return e;
						}
						if (in.op == sc::J || in.op == sc::LEAVE || in.op == sc::SWITCH)
							break;
					}
				}
			}
			return std::nullopt;
		}

		// The mission's own pass routine: walk up from the shared "passed" helpers (2 parameters, writing one of
		// the pass flags) through their callers to the first one that ends the script.
		std::optional<Entry> FindPassEntry(const Program& p, int32_t terminate)
		{
			const size_t n = p.functions.size();
			std::vector<bool> terminates(n), helper(n);
			std::vector<std::vector<std::pair<size_t, uint32_t>>> callers(n); // callee -> (caller, call site)
			for (const auto& in : p.instructions)
			{
				const size_t f = p.FunctionAt(in.address);
				if (in.op == sc::NATIVE && in.operand == terminate)
					terminates[f] = true;
				if (sc::IsGlobalStore(in) && (in.operand == kFlowPassed || in.operand == kStatsPassed.Index()) && p.functions[f].params == 2)
					helper[f] = true;
				if (in.op == sc::CALL && in.operand < static_cast<int64_t>(p.code.size()))
				{
					const size_t callee = p.FunctionAt(static_cast<uint32_t>(in.operand));
					callers[callee].emplace_back(f, in.address);
				}
			}
			// Ends the script through up to 3 levels of calls (pass routines call a cleanup that does).
			std::vector<std::set<size_t>> calls(n);
			for (const auto& in : p.instructions)
				if (in.op == sc::CALL && in.operand < static_cast<int64_t>(p.code.size()))
					calls[p.FunctionAt(in.address)].insert(p.FunctionAt(static_cast<uint32_t>(in.operand)));
			for (int pass = 0; pass < 3; ++pass)
			{
				auto next = terminates;
				for (size_t f = 0; f < n; ++f)
					next[f] = terminates[f] || std::any_of(calls[f].begin(), calls[f].end(), [&](size_t c) { return terminates[c]; });
				terminates = std::move(next);
			}

			// Following the code after the call at `site` (forward jumps followed, loops passed), the script ends
			// before the function returns: what a pass routine does, and what controllers calling the helpers do not.
			const auto index = [&](uint32_t address) {
				return static_cast<size_t>(std::lower_bound(p.instructions.begin(), p.instructions.end(), address,
				                               [](const sc::Instruction& in, uint32_t a) { return in.address < a; }) -
				                           p.instructions.begin());
			};
			const auto endsAfter = [&](uint32_t site) {
				size_t k = index(site);
				for (int step = 0; step < 400 && k < p.instructions.size(); ++step)
				{
					const auto& in = p.instructions[k];
					if (in.op == sc::NATIVE && in.operand == terminate)
						return true;
					if (in.op == sc::CALL && in.operand < static_cast<int64_t>(p.code.size()) &&
					    terminates[p.FunctionAt(static_cast<uint32_t>(in.operand))])
						return true;
					if (in.op == sc::J && in.operand > in.address)
					{
						k = index(static_cast<uint32_t>(in.operand));
						continue;
					}
					if (step && (in.op == sc::LEAVE || in.op == sc::SWITCH))
						return false;
					++k; // also past a loop's backward jump: its exit is the next instruction
				}
				return false;
			};

			std::vector<size_t> level;
			for (size_t f = 0; f < n; ++f)
				if (helper[f])
					level.push_back(f);
			std::set<size_t> seen(level.begin(), level.end());
			for (int depth = 0; depth < 4 && !level.empty(); ++depth)
			{
				std::vector<size_t> next;
				for (const size_t callee : level)
					for (const auto& [caller, site] : callers[callee])
					{
						if (endsAfter(site))
						{
							if (caller == 0) // the pass is one of main's cases
								return Entry{true, BlockStart(p, p.functions[0], site), 0};
							// A routine that runs one step per call (it returns, to be called again next frame) cannot be
							// jumped to: the mission's own loop has to run it.
							if (IsStageMachine(p, caller))
							{
								if (const auto stage = FindStage(p, caller, callers))
									return stage;
								for (const auto& [up, upSite] : callers[caller])
									if (up == 0)
									{
										Entry e{true, BlockStart(p, p.functions[0], upSite), 0};
										e.repeatInMain = true;
										return e;
									}
							}
							// A routine that runs to the end in one call: it registers the pass with the story flow and ends
							// the script (what the mission would have undone on the way, e.g. a fade, Recover() undoes).
							return Entry{false, p.functions[caller].start, p.functions[caller].params};
						}
						if (caller != 0 && seen.insert(caller).second)
							next.push_back(caller);
					}
				level = std::move(next);
			}
			return std::nullopt;
		}


		// Sets every visible stat of the current mission to a passing value; returns how many could not be.
		int ApplyGold(bool log = false)
		{
			int unsupported = 0;
			const int32_t count = std::min(kStatCount.Int(), kStatList.Size());
			for (int32_t k = 0; k < count; ++k)
			{
				const ml::Global entry = kStatList.At(k, 9);
				const int32_t id = entry.Int();
				if (id < 0 || id >= kStatDefs.Size())
					continue;
				const ml::Global def = kStatDefs.At(id, 13);
				if (def.Field(7).Bool())
					continue; // not shown on the results screen
				const int32_t type = def.Int();
				const int32_t target = def.Field(2).Int();
				const bool lowerIsBetter = def.Field(3).Bool();
				// Same test as the results screen: values below 1 fail for these types (times and counts).
				const int32_t least = type == 1 || type == 2 || type == 4 || type == 5 || type == 17 ? 1 : 0;
				const int32_t current = entry.Field(1).Int();
				const bool passes = current >= least && current != INT32_MAX && (lowerIsBetter ? current < target : current >= target);
				if (type == kStatTypeWatcherValue)
				{
					++unsupported;
					continue;
				}
				// A value that already passes stays as it is; otherwise the closest passing one (e.g. just under a
				// time limit) keeps the results believable.
				// Times (type 1, ms) keep running until the results screen reads them: leave a margin.
				const int32_t below = type == 1 ? std::min(target / 2, 3000) : 1;
				const int32_t value = passes ? current : lowerIsBetter ? target - below : std::max(target, least);
				if (value < least)
				{
					++unsupported;
					continue;
				}
				if (log)
					ml::Log("stat {} (type {}, target {}, lower {}): {} -> {}, state {}", id, type, target, lowerIsBetter, current, value, entry.Field(3).Int());
				entry.Field(1).Set(value);
				entry.Field(3).Set(0); // state: anything else voids the stat
			}
			return unsupported;
		}

		uint64_t g_goldUntil = 0;
		// repeatInMain: the thread and address main is sent to every frame, until the script ends or time is up.
		int32_t g_repeatThread = 0;
		uint32_t g_repeatAddress = 0;
		uint64_t g_repeatUntil = 0;
		// After a pass: the mission script ended part-way through what it does around its end, so what it left behind (a
		// fade to black, a character switch, a cutscene, script cameras, no player control) is undone if it lasts.
		int32_t g_recoverThread = 0;
		uint64_t g_recoverUntil = 0, g_stuckSince = 0;

		bool ThreadAlive(int32_t id)
		{
			const auto threads = ml::scripts::Threads();
			return std::any_of(threads.begin(), threads.end(), [&](const auto& t) { return t.id == id; });
		}

		void Recover()
		{
			if (!g_recoverThread)
				return;
			const uint64_t now = ml::TickMs();
			if (now > g_recoverUntil)
			{
				g_recoverThread = 0;
				return;
			}
			if (ThreadAlive(g_recoverThread))
				return;
			const Player player = PLAYER::PLAYER_ID();
			const bool faded = CAMERA::IS_SCREEN_FADED_OUT() != 0, switching = STREAMING::IS_PLAYER_SWITCH_IN_PROGRESS() != 0,
			           cutscene = CUTSCENE::IS_CUTSCENE_ACTIVE() != 0, noControl = !PLAYER::IS_PLAYER_CONTROL_ON(player);
			if (!faded && !switching && !cutscene && !noControl)
			{
				g_stuckSince = 0;
				return;
			}
			if (!g_stuckSince)
				g_stuckSince = now;
			if (now - g_stuckSince < 8000)
				return;
			ml::Log("after the pass: fade {}, switch {}, cutscene {}, no control {}: restored", faded, switching, cutscene, noControl);
			if (cutscene)
				CUTSCENE::STOP_CUTSCENE_IMMEDIATELY();
			if (switching)
				STREAMING::STOP_PLAYER_SWITCH();
			if (STREAMING::IS_NEW_LOAD_SCENE_ACTIVE())
				STREAMING::NEW_LOAD_SCENE_STOP();
			CAMERA::RENDER_SCRIPT_CAMS(false, false, 0, true, false, 0);
			PLAYER::SET_PLAYER_CONTROL(player, true, 0);
			HUD::DISPLAY_HUD(true);
			HUD::DISPLAY_RADAR(true);
			CAMERA::DO_SCREEN_FADE_IN(500);
			g_stuckSince = now;
		}
	}

	void Pass(bool gold)
	{
		for (const auto& thread : ml::scripts::Threads())
		{
			Program p;
			if (!Load(thread.id, p))
				continue;
			const int32_t terminate = ml::scripts::NativeIndex(thread.id, kTerminateThisThread);
			if (terminate < 0)
				continue;
			const auto entry = FindPassEntry(p, terminate);
			if (!entry)
				continue;

			ml::Log("mission {}: {} stats tracked", thread.name, kStatCount.Int());
			const int unsupported = gold ? ApplyGold(true) : 0;
			std::vector<int64_t> args(entry->params, 0);
			bool ok;
			if (entry->stage)
			{
				int64_t* stage = ml::scripts::Static(thread.id, entry->stageStatic);
				ok = stage != nullptr;
				if (ok)
					*stage = entry->stageValue;
				ml::Log("mission {}: stage static {} = {}", thread.name, entry->stageStatic, entry->stageValue);
			}
			else if (entry->inlineInMain)
			{
				ok = ml::Api().RedirectScript(thread.id, entry->address, nullptr, 0, 1) != 0;
				if (ok && entry->repeatInMain)
				{
					g_repeatThread = thread.id;
					g_repeatAddress = entry->address;
					g_repeatUntil = ml::TickMs() + 120000;
					ml::Log("mission {}: main sent to {} every frame", thread.name, entry->address);
				}
			}
			else
				ok = ml::Api().RedirectScript(thread.id, entry->address, args.data(), static_cast<int32_t>(args.size()), 0) != 0;
			ml::Log("mission {}: pass at {}{} -> {}", thread.name, entry->address, entry->inlineInMain ? " (in main)" : "", ok);
			if (!ok)
			{
				ml::Notify("無法讓任務 {} 直接完成", thread.name);
				return;
			}
			if (gold)
				g_goldUntil = ml::TickMs() + 60000;
			g_recoverThread = thread.id;
			g_recoverUntil = ml::TickMs() + 600000;
			g_stuckSince = 0;
			if (unsupported)
				ml::Notify("任務已完成；有 {} 個目標無法自動達成，獎牌可能不是金牌", unsupported);
			else if (gold)
				ml::Notify("任務已完成（全部目標達成）");
			else
				ml::Notify("任務已完成");
			return;
		}
		ml::Notify("目前沒有可以直接完成的任務");
	}

	void Frame()
	{
		Recover();
		if (g_repeatThread)
		{
			const auto threads = ml::scripts::Threads();
			const bool alive = std::any_of(threads.begin(), threads.end(), [](const auto& t) { return t.id == g_repeatThread; });
			if (!alive || ml::TickMs() > g_repeatUntil || !ml::Api().RedirectScript(g_repeatThread, g_repeatAddress, nullptr, 0, 1))
				g_repeatThread = 0;
		}
		if (!g_goldUntil)
			return;
		// Until the results screen read the stats (the pass flag is set and the list cleared afterwards).
		if (ml::TickMs() > g_goldUntil || (kStatsPassed.Bool() && kStatCount.Int() == 0))
		{
			g_goldUntil = 0;
			return;
		}
		ApplyGold();
	}
}
