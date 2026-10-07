#include "analysis.hpp"

#include <Zydis/Zydis.h>

#include <algorithm>
#include <atomic>
#include <cstring>
#include <thread>

namespace relocate
{
	namespace
	{
		uint64_t Mix(uint64_t h, uint64_t v)
		{
			h ^= v + 0x9E3779B97F4A7C15ull + (h << 6) + (h >> 2);
			return h;
		}

		struct Decoder
		{
			ZydisDecoder decoder;
			Decoder() { ZydisDecoderInit(&decoder, ZYDIS_MACHINE_MODE_LONG_64, ZYDIS_STACK_WIDTH_64); }
		};

		// A printable string at `rva` (ASCII or UTF-16, at least 4 characters), or "".
		std::string StringAt(const Image& image, uint32_t rva)
		{
			const uint8_t* p = image.At(rva, 8);
			if (!p || image.IsCode(rva))
				return {};
			std::string s;
			for (int i = 0; i < 256; ++i)
			{
				const uint8_t* c = image.At(rva + i);
				if (!c)
					return {};
				if (*c == 0)
					return s.size() >= 4 ? s : std::string();
				if (*c < 0x20 || *c > 0x7E)
					break;
				s.push_back(static_cast<char>(*c));
			}
			s.clear();
			for (int i = 0; i < 256; ++i) // UTF-16LE, ASCII range
			{
				const uint8_t* c = image.At(rva + 2 * i, 2);
				if (!c)
					return {};
				if (c[0] == 0 && c[1] == 0)
					return s.size() >= 4 ? "L\"" + s : std::string();
				if (c[1] != 0 || c[0] < 0x20 || c[0] > 0x7E)
					return {};
				s.push_back(static_cast<char>(c[0]));
			}
			return {};
		}
	}

	std::vector<Instruction> Decode(const Image& image, uint32_t begin, uint32_t end)
	{
		thread_local Decoder d;
		std::vector<Instruction> out;
		const uint8_t* code = image.At(begin, end - begin);
		if (!code)
			return out;
		const uint64_t base = image.Base();
		const uint64_t imageEnd = base + 0x10000000;
		ZydisDecodedInstruction ins;
		ZydisDecodedOperand ops[ZYDIS_MAX_OPERAND_COUNT];
		for (uint32_t at = 0; at < end - begin;)
		{
			if (!ZYAN_SUCCESS(ZydisDecoderDecodeFull(&d.decoder, code + at, end - begin - at, &ins, ops)))
				break;
			Instruction i;
			i.rva = begin + at;
			i.length = ins.length;
			i.mnemonic = static_cast<uint16_t>(ins.mnemonic);
			uint64_t shape = Mix(0, ins.mnemonic);
			for (uint8_t o = 0; o < ins.operand_count_visible; ++o)
			{
				const ZydisDecodedOperand& op = ops[o];
				shape = Mix(shape, op.type);
				if (op.type == ZYDIS_OPERAND_TYPE_REGISTER)
					shape = Mix(shape, op.reg.value);
				else if (op.type == ZYDIS_OPERAND_TYPE_MEMORY)
				{
					if (op.mem.base == ZYDIS_REGISTER_RIP)
					{
						ZyanU64 target = 0;
						if (ZYAN_SUCCESS(ZydisCalcAbsoluteAddress(&ins, &op, base + i.rva, &target)))
						{
							i.ref = Ref::Data;
							i.target = static_cast<uint32_t>(target - base);
						}
					}
					else
					{
						shape = Mix(shape, op.mem.base);
						shape = Mix(shape, op.mem.index);
						shape = Mix(shape, op.mem.scale);
						shape = Mix(shape, static_cast<uint64_t>(op.mem.disp.value)); // struct offsets
					}
				}
				else if (op.type == ZYDIS_OPERAND_TYPE_IMMEDIATE)
				{
					if (op.imm.is_relative)
					{
						ZyanU64 target = 0;
						if (ZYAN_SUCCESS(ZydisCalcAbsoluteAddress(&ins, &op, base + i.rva, &target)))
						{
							i.ref = ins.mnemonic == ZYDIS_MNEMONIC_CALL ? Ref::Call : Ref::Jump;
							i.target = static_cast<uint32_t>(target - base);
						}
					}
					else if (op.imm.value.u >= base && op.imm.value.u < imageEnd)
					{
						i.ref = Ref::Data;
						i.target = static_cast<uint32_t>(op.imm.value.u - base);
					}
					else
						shape = Mix(shape, op.imm.value.u);
				}
			}
			i.shape = shape;
			out.push_back(i);
			at += ins.length;
		}
		return out;
	}

	std::string Format(const Image& image, uint32_t rva)
	{
		thread_local Decoder d;
		static ZydisFormatter formatter = [] {
			ZydisFormatter f;
			ZydisFormatterInit(&f, ZYDIS_FORMATTER_STYLE_INTEL);
			return f;
		}();
		const uint8_t* code = image.At(rva, 1);
		if (!code)
			return "??";
		size_t available = 15;
		while (available > 1 && !image.At(rva, available))
			--available;
		ZydisDecodedInstruction ins;
		ZydisDecodedOperand ops[ZYDIS_MAX_OPERAND_COUNT];
		if (!ZYAN_SUCCESS(ZydisDecoderDecodeFull(&d.decoder, code, available, &ins, ops)))
			return "(bad)";
		char text[256];
		ZydisFormatterFormatInstruction(&formatter, &ins, ops, ins.operand_count_visible, text, sizeof(text), image.Base() + rva, ZYAN_NULL);
		return text;
	}

	bool Index::Build(const std::filesystem::path& file, std::string& error)
	{
		if (!image_.Load(file, error))
			return false;
		const auto& rf = image_.Functions();
		functions_.resize(rf.size());
		for (size_t i = 0; i < rf.size(); ++i)
			byBegin_[rf[i].begin] = static_cast<int>(i);

		// Features per function, in parallel; strings are collected per thread and numbered afterwards.
		std::vector<std::vector<std::pair<int, std::string>>> found(std::max(1u, std::thread::hardware_concurrency()));
		std::atomic<size_t> next = 0;
		std::vector<std::thread> workers;
		for (size_t t = 0; t < found.size(); ++t)
			workers.emplace_back([&, t] {
				for (size_t f; (f = next++) < rf.size();)
				{
					FunctionInfo& info = functions_[f];
					info.begin = rf[f].begin;
					info.end = rf[f].end;
					const auto ins = Decode(image_, info.begin, info.end);
					info.instructions = static_cast<uint32_t>(ins.size());
					uint64_t exact = 0;
					info.minhash.fill(UINT32_MAX);
					for (size_t k = 0; k < ins.size(); ++k)
					{
						const Instruction& i = ins[k];
						exact = Mix(exact, i.shape);
						if (k >= 2)
						{
							const uint64_t gram = Mix(Mix(Mix(0, ins[k - 2].mnemonic), ins[k - 1].mnemonic), i.mnemonic);
							for (int h = 0; h < 16; ++h)
								info.minhash[h] = std::min(info.minhash[h], static_cast<uint32_t>(Mix(gram, h * 0x51ED27u) >> 7));
						}
						if (i.ref == Ref::Call && image_.IsCode(i.target))
							info.callees.push_back(i.target);
						else if (i.ref == Ref::Data)
						{
							info.dataRefs.push_back(i.target);
							if (std::string s = StringAt(image_, i.target); !s.empty())
								found[t].emplace_back(static_cast<int>(f), std::move(s));
						}
					}
					info.exact = Mix(exact, ins.size());
				}
			});
		for (auto& w : workers)
			w.join();

		for (auto& list : found)
			for (auto& [f, s] : list)
			{
				auto [it, added] = stringIds_.emplace(s, static_cast<int>(strings_.size()));
				if (added)
				{
					strings_.push_back(s);
					stringUsers_.emplace_back();
				}
				auto& users = stringUsers_[it->second];
				if (users.empty() || users.back() != f)
					users.push_back(f);
				functions_[f].strings.push_back(it->second);
			}
		for (auto& users : stringUsers_)
		{
			std::sort(users.begin(), users.end());
			users.erase(std::unique(users.begin(), users.end()), users.end());
		}
		for (size_t f = 0; f < functions_.size(); ++f)
			for (const uint32_t c : functions_[f].callees)
			{
				const auto it = byBegin_.find(c);
				const int callee = it == byBegin_.end() ? -1 : it->second;
				functions_[f].calleeIndex.push_back(callee);
				if (callee >= 0 && (functions_[callee].callers.empty() || functions_[callee].callers.back() != static_cast<int>(f)))
					functions_[callee].callers.push_back(static_cast<int>(f));
			}
		return true;
	}

	int Index::StringId(const std::string& text) const
	{
		const auto it = stringIds_.find(text);
		return it == stringIds_.end() ? -1 : it->second;
	}

	int Index::FunctionAt(uint32_t begin) const
	{
		const auto it = byBegin_.find(begin);
		return it == byBegin_.end() ? -1 : it->second;
	}

	double Similarity(const FunctionInfo& a, const FunctionInfo& b)
	{
		int same = 0;
		for (int h = 0; h < 16; ++h)
			same += a.minhash[h] == b.minhash[h];
		return same / 16.0;
	}
}
