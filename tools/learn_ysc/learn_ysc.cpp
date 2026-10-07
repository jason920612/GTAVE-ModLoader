// Learns how legacy game scripts' global variables map onto Enhanced ones (research/phase0.md §26), from the same
// scripts of both games, and writes src/loader/convert/ysc_tables.inc.
//
// usage: learn_ysc <game folder (for zlib1.dll)> <legacy .ysc folder> <Enhanced .ysc folder> <out .inc>
//
// Both versions run the same bytecode with the same native hashes; Enhanced moved global variables. The instruction
// streams of each script pair are aligned (functions first, then the instructions of changed functions, with Myers'
// diff on the opcodes) and the operands of matching GLOBAL_U16 / GLOBAL_U24 instructions are paired up. Each legacy
// global gets the Enhanced index it was paired with most often.
#include <Windows.h>

#include <algorithm>
#include <atomic>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <map>
#include <mutex>
#include <thread>
#include <unordered_map>
#include <vector>

#include "../../src/loader/convert/resource.hpp"

using loader::convert::Bytes;
using loader::convert::Get;

namespace
{
	struct Instruction
	{
		int kind;            // opcode; GLOBAL_U16/U24 load/store/address share a kind per access type
		int64_t global = -1; // global index for GLOBAL_* instructions
	};

	// Opcode size (the same table as src/loader/convert/ysc.cpp).
	uint32_t InstructionSize(const std::vector<uint8_t>& c, uint32_t ip)
	{
		const uint8_t op = c[ip];
		if (op <= 36 || (op >= 42 && op <= 43) || (op >= 47 && op <= 51) || op == 63 || op == 102 || op == 103 ||
		    (op >= 108 && op <= 111) || op >= 112)
			return 1;
		if (op == 37 || (op >= 52 && op <= 62) || (op >= 64 && op <= 66) || (op >= 104 && op <= 107))
			return 2;
		if (op == 38)
			return 3;
		if (op == 39 || op == 44 || op == 93 || (op >= 94 && op <= 100))
			return 4;
		if (op == 40 || op == 41)
			return 5;
		if (op == 45)
			return 5 + (ip + 4 < c.size() ? c[ip + 4] : 0);
		if (op == 46 || (op >= 67 && op <= 92))
			return 3;
		if (op == 101)
			return 2 + 6 * (ip + 1 < c.size() ? c[ip + 1] : 0);
		return 1;
	}

	// The script's code (pages joined) as instructions; empty when the file is not a script.
	std::vector<Instruction> LoadScript(const std::filesystem::path& file)
	{
		std::ifstream in(file, std::ios::binary);
		Bytes data((std::istreambuf_iterator<char>(in)), {});
		loader::convert::Resource res;
		std::string error;
		if (!loader::convert::Read(data, res, error) || res.virtualBlock.size() < 0x80)
			return {};
		const Bytes& v = res.virtualBlock;
		const auto at = [&](uint64_t p, size_t n) -> const uint8_t* {
			const uint32_t q = static_cast<uint32_t>(p);
			const Bytes* b = (q >> 28) == 5 ? &res.virtualBlock : (q >> 28) == 6 ? &res.physicalBlock : nullptr;
			const size_t o = q & 0x0FFFFFFF;
			return b && o + n <= b->size() ? b->data() + o : nullptr;
		};
		const uint32_t size = Get<uint32_t>(v, 0x1C);
		const uint32_t pages = (size + 0x3FFF) / 0x4000;
		const uint8_t* table = at(Get<uint64_t>(v, 0x10), size_t(pages) * 8);
		if (!table)
			return {};
		std::vector<uint8_t> code;
		for (uint32_t i = 0; i < pages; ++i)
		{
			uint64_t p;
			memcpy(&p, table + 8 * i, 8);
			const uint32_t n = std::min<uint32_t>(0x4000, size - i * 0x4000);
			const uint8_t* page = at(p, n);
			if (!page)
				return {};
			code.insert(code.end(), page, page + n);
		}
		std::vector<Instruction> out;
		for (uint32_t ip = 0; ip < code.size(); ip += InstructionSize(code, ip))
		{
			const uint8_t op = code[ip];
			Instruction i{op};
			const bool u16 = op >= 82 && op <= 84, u24 = op >= 97 && op <= 99;
			if ((u16 || u24) && ip + 3 < code.size())
			{
				i.kind = 1000 + (u16 ? op - 82 : op - 97);
				i.global = code[ip + 1] | code[ip + 2] << 8 | (u24 ? code[ip + 3] << 16 : 0);
			}
			out.push_back(i);
		}
		return out;
	}

	// Myers' diff (linear space): appends the index pairs of matching elements of a[0..n) and b[0..m).
	class Differ
	{
	public:
		Differ(const int* a, const int* b, int maxD) : a_(a), b_(b), maxD_(maxD) {}

		bool Run(int n, int m, std::vector<std::pair<int, int>>& out)
		{
			out_ = &out;
			return Diff(0, n, 0, m);
		}

	private:
		const int* a_;
		const int* b_;
		int maxD_;
		std::vector<std::pair<int, int>>* out_ = nullptr;
		std::vector<int> vf_, vb_;

		bool Diff(int a0, int a1, int b0, int b1)
		{
			while (a0 < a1 && b0 < b1 && a_[a0] == b_[b0])
				out_->emplace_back(a0++, b0++);
			std::vector<std::pair<int, int>> tail;
			while (a0 < a1 && b0 < b1 && a_[a1 - 1] == b_[b1 - 1])
				tail.emplace_back(--a1, --b1);
			bool ok = true;
			if (a0 < a1 && b0 < b1)
			{
				int x, y, u, v;
				const int d = MiddleSnake(a0, a1, b0, b1, x, y, u, v);
				if (d < 0)
					ok = false;
				else if (d > 1)
				{
					ok = Diff(a0, x, b0, y);
					for (int i = 0; i < u - x; ++i)
						out_->emplace_back(x + i, y + i);
					ok = ok && Diff(u, a1, v, b1);
				}
			}
			out_->insert(out_->end(), tail.rbegin(), tail.rend());
			return ok;
		}

		// The middle snake of the shortest edit script: (x, y) to (u, v); returns its D, or -1 past maxD.
		int MiddleSnake(int a0, int a1, int b0, int b1, int& sx, int& sy, int& su, int& sv)
		{
			const int n = a1 - a0, m = b1 - b0, delta = n - m, max = (n + m + 1) / 2;
			const bool odd = delta & 1;
			const int off = max + 1;
			vf_.assign(2 * max + 3, 0);
			vb_.assign(2 * max + 3, 0);
			for (int d = 0; d <= max; ++d)
			{
				if (d > maxD_)
					return -1;
				for (int k = -d; k <= d; k += 2)
				{
					int x = (k == -d || (k != d && vf_[off + k - 1] < vf_[off + k + 1])) ? vf_[off + k + 1] : vf_[off + k - 1] + 1;
					int y = x - k;
					const int xs = x, ys = y;
					while (x < n && y < m && a_[a0 + x] == b_[b0 + y])
						++x, ++y;
					vf_[off + k] = x;
					const int kb = delta - k;
					if (odd && kb >= -(d - 1) && kb <= d - 1 && vf_[off + k] + vb_[off + kb] >= n)
					{
						sx = a0 + xs, sy = b0 + ys, su = a0 + x, sv = b0 + y;
						return 2 * d - 1;
					}
				}
				for (int k = -d; k <= d; k += 2)
				{
					int x = (k == -d || (k != d && vb_[off + k - 1] < vb_[off + k + 1])) ? vb_[off + k + 1] : vb_[off + k - 1] + 1;
					int y = x - k;
					const int xs = x, ys = y;
					while (x < n && y < m && a_[a1 - 1 - x] == b_[b1 - 1 - y])
						++x, ++y;
					vb_[off + k] = x;
					const int kf = delta - k;
					if (!odd && kf >= -d && kf <= d && vf_[off + kf] + vb_[off + k] >= n)
					{
						sx = a1 - x, sy = b1 - y, su = a1 - xs, sv = b1 - ys;
						return 2 * d;
					}
				}
			}
			return -1;
		}
	};

	std::vector<std::pair<int, int>> Match(const std::vector<int>& a, const std::vector<int>& b, int maxD)
	{
		std::vector<std::pair<int, int>> pairs;
		Differ(a.data(), b.data(), maxD).Run(static_cast<int>(a.size()), static_cast<int>(b.size()), pairs);
		return pairs;
	}

	// Functions (each starts at ENTER) as [begin, end) instruction ranges, and a hash of their opcodes.
	struct Function
	{
		size_t begin, end;
		int hash;
	};
	std::vector<Function> Functions(const std::vector<Instruction>& ins)
	{
		std::vector<Function> out;
		for (size_t i = 0; i < ins.size(); ++i)
			if (ins[i].kind == 45 || out.empty())
				out.push_back({i, i, 0});
		for (size_t f = 0; f < out.size(); ++f)
		{
			out[f].end = f + 1 < out.size() ? out[f + 1].begin : ins.size();
			uint32_t h = 2166136261u;
			for (size_t i = out[f].begin; i < out[f].end; ++i)
				h = (h ^ static_cast<uint32_t>(ins[i].kind)) * 16777619u;
			out[f].hash = static_cast<int>(h);
		}
		return out;
	}

	using Counts = std::unordered_map<int64_t, std::unordered_map<int64_t, int>>;

	void Pair(const std::vector<Instruction>& a, size_t ab, size_t ae, const std::vector<Instruction>& b, size_t bb, size_t be, Counts& counts)
	{
		std::vector<int> ka, kb;
		for (size_t i = ab; i < ae; ++i)
			ka.push_back(a[i].kind);
		for (size_t i = bb; i < be; ++i)
			kb.push_back(b[i].kind);
		for (const auto& [i, j] : Match(ka, kb, 20000))
			if (a[ab + i].global >= 0 && b[bb + j].global >= 0)
				++counts[a[ab + i].global][b[bb + j].global];
	}

	// Pairs the globals of two versions of a script.
	void Learn(const std::vector<Instruction>& a, const std::vector<Instruction>& b, Counts& counts)
	{
		const auto fa = Functions(a), fb = Functions(b);
		std::vector<int> ha, hb;
		for (const auto& f : fa)
			ha.push_back(f.hash);
		for (const auto& f : fb)
			hb.push_back(f.hash);
		const auto matched = Match(ha, hb, 1 << 30);
		size_t pa = 0, pb = 0;
		const auto gap = [&](size_t ea, size_t eb) {
			// Changed functions between two matched ones: paired by position when the counts agree.
			if (ea - pa == eb - pb)
				for (size_t k = 0; k < ea - pa; ++k)
					Pair(a, fa[pa + k].begin, fa[pa + k].end, b, fb[pb + k].begin, fb[pb + k].end, counts);
		};
		for (const auto& [i, j] : matched)
		{
			gap(i, j);
			const Function &x = fa[i], &y = fb[j];
			for (size_t k = 0; k < x.end - x.begin; ++k)
				if (a[x.begin + k].global >= 0)
					++counts[a[x.begin + k].global][b[y.begin + k].global];
			pa = i + 1, pb = j + 1;
		}
		gap(fa.size(), fb.size());
	}
}

int wmain(int argc, wchar_t** argv)
{
	if (argc != 5)
	{
		fprintf(stderr, "usage: learn_ysc <game folder> <legacy .ysc folder> <Enhanced .ysc folder> <out .inc>\n");
		return 1;
	}
	SetDllDirectoryW(argv[1]); // zlib1.dll ships with the game
	const std::filesystem::path legacyDir = argv[2], enhancedDir = argv[3], out = argv[4];
	std::vector<std::filesystem::path> names;
	for (const auto& e : std::filesystem::directory_iterator(legacyDir))
		if (std::filesystem::exists(enhancedDir / e.path().filename()))
			names.push_back(e.path().filename());

	Counts total;
	std::mutex mutex;
	std::atomic<size_t> next = 0, done = 0;
	std::vector<std::thread> workers;
	for (unsigned t = 0; t < (std::max)(1u, std::thread::hardware_concurrency()); ++t)
		workers.emplace_back([&] {
			for (size_t i; (i = next++) < names.size();)
			{
				const auto a = LoadScript(legacyDir / names[i]), b = LoadScript(enhancedDir / names[i]);
				Counts counts;
				if (!a.empty() && !b.empty())
					Learn(a, b, counts);
				std::lock_guard lock(mutex);
				for (const auto& [g, targets] : counts)
					for (const auto& [t2, c] : targets)
						total[g][t2] += c;
				if (++done % 100 == 0)
					fprintf(stderr, "%zu/%zu scripts, %zu globals\n", done.load(), names.size(), total.size());
			}
		});
	for (auto& w : workers)
		w.join();

	std::map<int64_t, int64_t> map;
	int ambiguous = 0;
	for (const auto& [g, targets] : total)
	{
		std::vector<std::pair<int, int64_t>> sorted;
		for (const auto& [t2, c] : targets)
			sorted.emplace_back(c, t2);
		std::sort(sorted.rbegin(), sorted.rend());
		if (sorted.size() > 1 && sorted[1].first * 4 > sorted[0].first)
			++ambiguous;
		map[g] = sorted[0].second;
	}
	std::ofstream f(out, std::ios::binary);
	f << "// Generated by tools/learn_ysc (research/phase0.md \xC2\xA7" "26).\n#ifdef ML_YSC_GLOBALS\n"
	     "// {legacy global index, Enhanced global index}, sorted\n";
	for (const auto& [a, b] : map)
		f << "{" << a << ", " << b << "},\n";
	f << "#endif\n";
	printf("%zu scripts, %zu globals, %d ambiguous\n", names.size(), map.size(), ambiguous);
	return 0;
}
