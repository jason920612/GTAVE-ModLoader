#include "mission.hpp"

#include <algorithm>
#include <climits>
#include <map>
#include <optional>
#include <set>
#include <string>
#include <vector>

#include <modloader/modloader.hpp>
#include <modloader/script.hpp>

namespace mission
{
	namespace
	{
		namespace sc = ml::script;

		// Script globals of build 0x6aa45f10 (research/phase0.md §22).
		constexpr uint32_t kFlowPassed = 65082;  // written by the story missions' shared "passed" helper
		constexpr uint32_t kStatsPassed = 65074; // mission_stat_watcher shows the results once it is set
		constexpr uint32_t kStatCount = 77175;   // stats the current mission tracks
		constexpr uint32_t kStatList = 77176;    // array: entries of 9 slots (stat id, value, ..., state at +3)
		constexpr uint32_t kStatDefs = 65305;    // array: definitions of 13 slots (type, ?, target, lower is better,
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
				if (sc::IsGlobalStore(in) && (in.operand == kFlowPassed || in.operand == kStatsPassed) && p.functions[f].params == 2)
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
							return Entry{false, p.functions[caller].start, p.functions[caller].params};
						}
						if (caller != 0 && seen.insert(caller).second)
							next.push_back(caller);
					}
				level = std::move(next);
			}
			return std::nullopt;
		}

		// Script integers are 32-bit; the upper half of a slot can hold anything.
		int32_t Read(uint32_t index)
		{
			const int64_t* g = ml::scripts::Global(index);
			return g ? static_cast<int32_t>(*g) : 0;
		}

		void Write(uint32_t index, int32_t value)
		{
			if (auto* g = reinterpret_cast<int32_t*>(ml::scripts::Global(index)))
				*g = value;
		}

		// Sets every visible stat of the current mission to a passing value; returns how many could not be.
		int ApplyGold(bool log = false)
		{
			int unsupported = 0;
			const int32_t count = std::min(Read(kStatCount), Read(kStatList));
			for (int32_t k = 0; k < count; ++k)
			{
				const uint32_t entry = kStatList + 1 + static_cast<uint32_t>(9 * k);
				const int32_t id = Read(entry);
				if (id < 0 || id >= Read(kStatDefs))
					continue;
				const uint32_t def = kStatDefs + 1 + static_cast<uint32_t>(13 * id);
				if (Read(def + 7))
					continue; // not shown on the results screen
				const int32_t type = Read(def);
				const int32_t target = Read(def + 2);
				const bool lowerIsBetter = Read(def + 3) != 0;
				// Same test as the results screen: values below 1 fail for these types (times and counts).
				const int32_t least = type == 1 || type == 2 || type == 4 || type == 5 || type == 17 ? 1 : 0;
				const int32_t current = Read(entry + 1);
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
					ml::Log("stat {} (type {}, target {}, lower {}): {} -> {}, state {}", id, type, target, lowerIsBetter, Read(entry + 1), value, Read(entry + 3));
				Write(entry + 1, value);
				Write(entry + 3, 0); // state: anything else voids the stat
			}
			return unsupported;
		}

		uint64_t g_goldUntil = 0;
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

			ml::Log("mission {}: {} stats tracked", thread.name, Read(kStatCount));
			const int unsupported = gold ? ApplyGold(true) : 0;
			std::vector<int64_t> args(entry->params, 0);
			bool ok;
			if (entry->inlineInMain)
				ok = ml::Api().RedirectScript(thread.id, entry->address, nullptr, 0, 1) != 0;
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
		if (!g_goldUntil)
			return;
		// Until the results screen read the stats (the pass flag is set and the list cleared afterwards).
		if (ml::TickMs() > g_goldUntil || (Read(kStatsPassed) && Read(kStatCount) == 0))
		{
			g_goldUntil = 0;
			return;
		}
		ApplyGold();
	}
}
