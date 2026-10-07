#include "match.hpp"

#include <algorithm>
#include <format>
#include <map>
#include <unordered_map>

namespace relocate
{
	const char* Name(How how)
	{
		switch (how)
		{
		case How::String: return "string";
		case How::Exact: return "exact";
		case How::Graph: return "call graph";
		case How::Similar: return "similar";
		default: return "none";
		}
	}

	namespace
	{
		// Longest common subsequence of two sequences (index pairs), by dynamic programming over a band; good enough for
		// function bodies (thousands of instructions).
		std::vector<std::pair<int, int>> Align(const std::vector<uint64_t>& a, const std::vector<uint64_t>& b)
		{
			std::vector<std::pair<int, int>> pairs;
			size_t i = 0, j = 0;
			while (i < a.size() && j < b.size() && a[i] == b[j])
				pairs.emplace_back(static_cast<int>(i++), static_cast<int>(j++));
			size_t ea = a.size(), eb = b.size();
			std::vector<std::pair<int, int>> tail;
			while (ea > i && eb > j && a[ea - 1] == b[eb - 1])
				tail.emplace_back(static_cast<int>(--ea), static_cast<int>(--eb));
			const size_t n = ea - i, m = eb - j;
			if (n && m && n * m <= 64'000'000)
			{
				std::vector<uint16_t> dp((n + 1) * (m + 1), 0);
				const auto at = [&](size_t x, size_t y) -> uint16_t& { return dp[x * (m + 1) + y]; };
				for (size_t x = n; x-- > 0;)
					for (size_t y = m; y-- > 0;)
						at(x, y) = a[i + x] == b[j + y] ? static_cast<uint16_t>(at(x + 1, y + 1) + 1) : std::max(at(x + 1, y), at(x, y + 1));
				for (size_t x = 0, y = 0; x < n && y < m;)
				{
					if (a[i + x] == b[j + y])
						pairs.emplace_back(static_cast<int>(i + x++), static_cast<int>(j + y++));
					else if (at(x + 1, y) >= at(x, y + 1))
						++x;
					else
						++y;
				}
			}
			pairs.insert(pairs.end(), tail.rbegin(), tail.rend());
			return pairs;
		}

		bool CloseSize(const FunctionInfo& a, const FunctionInfo& b)
		{
			const double r = (a.instructions + 1.0) / (b.instructions + 1.0);
			return r > 0.5 && r < 2.0;
		}
	}

	Matcher::Matcher(const Index& before, const Index& after) : a_(before), b_(after)
	{
		match_.assign(a_.Functions().size(), -1);
		reverse_.assign(b_.Functions().size(), -1);
		how_.assign(a_.Functions().size(), How::None);
		Strings();
		Exact();
		while (Propagate())
		{
		}
	}

	bool Matcher::Pair(int a, int b, How how)
	{
		if (a < 0 || b < 0 || match_[a] >= 0 || reverse_[b] >= 0)
			return false;
		match_[a] = b;
		reverse_[b] = a;
		how_[a] = how;
		return true;
	}

	size_t Matcher::Count(How how) const
	{
		return static_cast<size_t>(std::count(how_.begin(), how_.end(), how));
	}

	// Strings used by exactly one function in both builds pair those functions.
	void Matcher::Strings()
	{
		for (size_t s = 0; s < a_.Strings().size(); ++s)
		{
			const auto& users = a_.StringUsers(static_cast<int>(s));
			if (users.size() != 1)
				continue;
			const int other = b_.StringId(a_.Strings()[s]);
			if (other < 0 || b_.StringUsers(other).size() != 1)
				continue;
			Pair(users[0], b_.StringUsers(other)[0], How::String);
		}
	}

	// Functions whose instruction shapes are identical and unique in both builds.
	void Matcher::Exact()
	{
		std::unordered_map<uint64_t, int> inA, inB;
		for (size_t f = 0; f < a_.Functions().size(); ++f)
		{
			auto [it, added] = inA.emplace(a_.Functions()[f].exact, static_cast<int>(f));
			if (!added)
				it->second = -1;
		}
		for (size_t f = 0; f < b_.Functions().size(); ++f)
		{
			auto [it, added] = inB.emplace(b_.Functions()[f].exact, static_cast<int>(f));
			if (!added)
				it->second = -1;
		}
		for (const auto& [hash, f] : inA)
			if (f >= 0 && a_.Functions()[f].instructions >= 8)
				if (const auto it = inB.find(hash); it != inB.end() && it->second >= 0)
					Pair(f, it->second, How::Exact);
	}

	// Unmatched callees (in call order) and callers of matched pairs: aligned by their instruction shapes, or by
	// position when both lists have the same length and the functions are of a similar size.
	bool Matcher::Propagate()
	{
		bool changed = false;
		const auto pairLists = [&](const std::vector<int>& la, const std::vector<int>& lb) {
			std::vector<int> ua, ub;
			for (const int f : la)
				if (f >= 0 && match_[f] < 0 && std::find(ua.begin(), ua.end(), f) == ua.end())
					ua.push_back(f);
			for (const int f : lb)
				if (f >= 0 && reverse_[f] < 0 && std::find(ub.begin(), ub.end(), f) == ub.end())
					ub.push_back(f);
			if (ua.empty() || ub.empty())
				return;
			if (ua.size() == ub.size())
			{
				for (size_t k = 0; k < ua.size(); ++k)
					if (CloseSize(a_.Functions()[ua[k]], b_.Functions()[ub[k]]) && Similarity(a_.Functions()[ua[k]], b_.Functions()[ub[k]]) >= 0.25)
						changed |= Pair(ua[k], ub[k], How::Graph);
				return;
			}
			for (const int x : ua) // best similar candidate, when clearly the best
			{
				int best = -1;
				double score = 0, second = 0;
				for (const int y : ub)
				{
					if (reverse_[y] >= 0 || !CloseSize(a_.Functions()[x], b_.Functions()[y]))
						continue;
					const double s = Similarity(a_.Functions()[x], b_.Functions()[y]) + (a_.Functions()[x].exact == b_.Functions()[y].exact ? 1 : 0);
					if (s > score)
						second = score, score = s, best = y;
					else if (s > second)
						second = s;
				}
				if (best >= 0 && score >= 0.5 && score - second >= 0.2)
					changed |= Pair(x, best, How::Graph);
			}
		};
		for (size_t f = 0; f < a_.Functions().size(); ++f)
		{
			const int g = match_[f];
			if (g < 0)
				continue;
			pairLists(a_.Functions()[f].calleeIndex, b_.Functions()[g].calleeIndex);
			pairLists(a_.Functions()[f].callers, b_.Functions()[g].callers);
		}
		return changed;
	}

	// For an unmatched function: the most similar unmatched function near where its matched neighbours went.
	int Matcher::Similar(int function)
	{
		const FunctionInfo& f = a_.Functions()[function];
		std::vector<int> candidates;
		for (const int c : f.callers)
			if (match_[c] >= 0)
				for (const int x : b_.Functions()[match_[c]].calleeIndex)
					if (x >= 0)
						candidates.push_back(x);
		for (const int c : f.calleeIndex)
			if (c >= 0 && match_[c] >= 0)
				for (const int x : b_.Functions()[match_[c]].callers)
					candidates.push_back(x);
		if (candidates.empty()) // nothing to go by: every function of a similar size
			for (size_t x = 0; x < b_.Functions().size(); ++x)
				candidates.push_back(static_cast<int>(x));
		std::sort(candidates.begin(), candidates.end());
		candidates.erase(std::unique(candidates.begin(), candidates.end()), candidates.end());
		int best = -1;
		double score = 0, second = 0;
		for (const int y : candidates)
		{
			if (reverse_[y] >= 0 || !CloseSize(f, b_.Functions()[y]))
				continue;
			const double s = Similarity(f, b_.Functions()[y]);
			if (s > score)
				second = score, score = s, best = y;
			else if (s > second)
				second = s;
		}
		if (best >= 0 && score >= 0.4 && score - second >= 0.1)
		{
			Pair(function, best, How::Similar);
			return best;
		}
		return -1;
	}

	Matcher::Result Matcher::InFunction(uint32_t rva, int function)
	{
		Result r;
		int other = match_[function];
		if (other < 0)
			other = Similar(function);
		if (other < 0)
		{
			r.note = "function not matched";
			return r;
		}
		r.how = how_[function];
		const FunctionInfo& fa = a_.Functions()[function];
		const FunctionInfo& fb = b_.Functions()[other];
		const auto ia = Decode(a_.Img(), fa.begin, fa.end), ib = Decode(b_.Img(), fb.begin, fb.end);
		std::vector<uint64_t> sa, sb;
		for (const auto& i : ia)
			sa.push_back(i.shape);
		for (const auto& i : ib)
			sb.push_back(i.shape);
		size_t at = 0;
		while (at + 1 < ia.size() && ia[at + 1].rva <= rva)
			++at;
		for (const auto& [x, y] : Align(sa, sb))
			if (static_cast<size_t>(x) == at)
			{
				r.ok = true;
				r.rva = ib[y].rva + (rva - ia[x].rva);
				r.note = std::format("function {:#x} -> {:#x}, instruction {} -> {}", fa.begin, fb.begin, x, y);
				return r;
			}
		r.note = std::format("function {:#x} -> {:#x}, but the instruction changed", fa.begin, fb.begin);
		return r;
	}

	Matcher::Result Matcher::InData(uint32_t rva)
	{
		// Every matched function referencing the address votes for what the same instruction references in the new build.
		std::map<uint32_t, int> votes;
		Result r;
		for (size_t f = 0; f < a_.Functions().size(); ++f)
		{
			const FunctionInfo& fa = a_.Functions()[f];
			if (std::find(fa.dataRefs.begin(), fa.dataRefs.end(), rva) == fa.dataRefs.end())
				continue;
			int other = match_[f];
			if (other < 0)
				continue;
			const FunctionInfo& fb = b_.Functions()[other];
			const auto ia = Decode(a_.Img(), fa.begin, fa.end), ib = Decode(b_.Img(), fb.begin, fb.end);
			std::vector<uint64_t> sa, sb;
			for (const auto& i : ia)
				sa.push_back(i.shape);
			for (const auto& i : ib)
				sb.push_back(i.shape);
			for (const auto& [x, y] : Align(sa, sb))
				if (ia[x].ref == Ref::Data && ia[x].target == rva && ib[y].ref == Ref::Data)
					++votes[ib[y].target];
		}
		if (votes.empty())
		{
			r.note = "no matched code references it";
			return r;
		}
		const auto best = std::max_element(votes.begin(), votes.end(), [](const auto& x, const auto& y) { return x.second < y.second; });
		int total = 0;
		for (const auto& [t, v] : votes)
			total += v;
		r.ok = true;
		r.rva = best->first;
		r.how = How::Graph;
		r.note = std::format("{} of {} references agree", best->second, total);
		return r;
	}

	Matcher::Result Matcher::Relocate(uint32_t rva)
	{
		const int function = a_.Img().FunctionOf(rva);
		return function >= 0 ? InFunction(rva, function) : InData(rva);
	}
}
