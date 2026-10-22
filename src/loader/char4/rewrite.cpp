#include "rewrite.hpp"

#include <algorithm>
#include <cctype>
#include <array>
#include <format>
#include <map>
#include <optional>
#include <set>
#include <unordered_set>

namespace loader::char4
{
	namespace
	{
		constexpr uint32_t kNoLabel = UINT32_MAX;
		constexpr uint32_t kNewLabels = 0x80000000; // labels of new code (original code is labelled by its address)
		constexpr uint8_t kIor = 32, kInot = 6, kIsBitSet = 129;

		// One instruction (or a label: no bytes) of code being laid out. `rel`: the s16 fields of the bytes that hold a
		// relative jump (from the end of the field) to a label.
		struct Piece
		{
			uint32_t label = kNoLabel;
			Bytes bytes;
			std::vector<std::pair<uint32_t, uint32_t>> rel;
		};

		struct Asm
		{
			std::vector<Piece> pieces;
			uint32_t next = kNewLabels;

			uint32_t NewLabel() { return next++; }
			void Mark(uint32_t label) { pieces.push_back({label}); }
			void Raw(std::initializer_list<uint8_t> b) { pieces.push_back({kNoLabel, Bytes(b)}); }
			void Jump(uint8_t o, uint32_t label) { pieces.push_back({kNoLabel, Bytes{o, 0, 0}, {{1, label}}}); }
			void Call(uint32_t a) { Raw({op::Call, uint8_t(a), uint8_t(a >> 8), uint8_t(a >> 16)}); }
			void Native(uint16_t index, uint8_t args, uint8_t rets) { Raw({op::Native, uint8_t(args << 2 | rets), uint8_t(index >> 8), uint8_t(index)}); }
			void Enter(uint8_t params, uint16_t frame) { Raw({op::Enter, params, uint8_t(frame), uint8_t(frame >> 8), 0}); }
			void Leave(uint8_t params, uint8_t rets) { Raw({op::Leave, params, rets}); }
			void Local(uint8_t i) { Raw({op::LocalU8Load, i}); }
			void Push(int32_t v)
			{
				if (v >= -1 && v <= 7)
					Raw({uint8_t(op::Push0 + v)});
				else if (v >= 0 && v <= 255)
					Raw({op::PushU8, uint8_t(v)});
				else
					Raw({op::PushU32, uint8_t(v), uint8_t(v >> 8), uint8_t(v >> 16), uint8_t(uint32_t(v) >> 24)});
			}
		};

		// Lays the pieces out from `at`: no instruction reaches the last byte of a code page (NOP padding, as the
		// game's compiler does; the interpreter goes on into the next page from a NOP). Returns the position of every
		// label.
		bool Layout(const std::vector<Piece>& pieces, uint32_t at, Bytes& out, std::unordered_map<uint32_t, uint32_t>& where, std::string& error)
		{
			std::vector<uint32_t> pos(pieces.size());
			std::vector<size_t> pending; // labels waiting for the next instruction
			uint32_t p = at;
			for (size_t i = 0; i < pieces.size(); ++i)
			{
				const auto size = static_cast<uint32_t>(pieces[i].bytes.size());
				if (size)
				{
					if (size >= kCodePage - 1)
					{
						error = "instruction larger than a code page";
						return false;
					}
					if (p % kCodePage + size > kCodePage - 1)
						p = (p / kCodePage + 1) * kCodePage;
					for (const size_t k : pending)
						pos[k] = p;
					pending.clear();
				}
				else
					pending.push_back(i);
				pos[i] = p;
				p += size;
			}
			for (const size_t k : pending)
				pos[k] = p;
			for (size_t i = 0; i < pieces.size(); ++i)
				if (pieces[i].label != kNoLabel)
					where.try_emplace(pieces[i].label, pos[i]);
			out.assign(p - at, op::Nop);
			for (size_t i = 0; i < pieces.size(); ++i)
			{
				const Piece& piece = pieces[i];
				std::copy(piece.bytes.begin(), piece.bytes.end(), out.begin() + (pos[i] - at));
				for (const auto& [field, label] : piece.rel)
				{
					const auto it = where.find(label);
					if (it == where.end())
					{
						error = std::format("jump to a missing label {}", label);
						return false;
					}
					const int64_t offset = int64_t(it->second) - (int64_t(pos[i]) + field + 2);
					if (offset < INT16_MIN || offset > INT16_MAX)
					{
						error = "jump too far after the change";
						return false;
					}
					out[pos[i] - at + field] = uint8_t(offset);
					out[pos[i] - at + field + 1] = uint8_t(offset >> 8);
				}
			}
			return true;
		}

		// The function [start, end) as pieces labelled by their addresses, with its jumps and SWITCH tables as
		// relative fields.
		std::vector<Piece> Pieces(const Bytes& code, uint32_t start, uint32_t end)
		{
			std::vector<Piece> out;
			for (const Instruction& i : Decode(code, start, end))
			{
				Piece p{i.at, Bytes(code.begin() + i.at, code.begin() + i.at + i.size)};
				if (op::IsJump(i.op))
					p.rel.push_back({1, JumpTarget(code, i.at)});
				else if (i.op == op::Switch)
				{
					const auto cases = SwitchCases(code, i.at);
					for (uint32_t k = 0; k < cases.size(); ++k)
						p.rel.push_back({2 + 6 * k + 4, cases[k].target});
				}
				out.push_back(std::move(p));
			}
			return out;
		}

		// Moves the function [start, end) to the end of the code as `pieces` (its new body) and makes the old body
		// forward every call to it: ENTER as before, the parameters pushed, CALL, LEAVE.
		bool Relocate(Program& p, uint32_t start, uint32_t end, const std::vector<Piece>& pieces, std::string& error)
		{
			Bytes body;
			std::unordered_map<uint32_t, uint32_t> where;
			const auto at = static_cast<uint32_t>(p.code.size());
			if (!Layout(pieces, at, body, where, error))
				return false;
			const uint32_t moved = where.at(start);
			const uint8_t params = p.code[start + 1];
			uint8_t rets = 0;
			for (const Instruction& i : Decode(p.code, start, end))
				if (i.op == op::Leave)
				{
					rets = p.code[i.at + 2];
					break;
				}
			Asm f;
			f.pieces.push_back({kNoLabel, Bytes(p.code.begin() + start, p.code.begin() + start + 5 + p.code[start + 4])});
			for (uint8_t i = 0; i < params; ++i)
				f.Local(i);
			f.Call(moved);
			f.Leave(params, rets);
			Bytes forward;
			std::unordered_map<uint32_t, uint32_t> unused;
			if (!Layout(f.pieces, start, forward, unused, error))
				return false;
			if (start + forward.size() > end)
			{
				error = std::format("function @{} too short for a forwarder", start);
				return false;
			}
			if (at + body.size() >= (1u << 24))
			{
				error = "code too large";
				return false;
			}
			p.code.insert(p.code.end(), body.begin(), body.end());
			std::copy(forward.begin(), forward.end(), p.code.begin() + start);
			std::fill(p.code.begin() + start + forward.size(), p.code.begin() + end, op::Nop);
			return true;
		}

		// Appends a new function; returns its address.
		bool Append(Program& p, const Asm& a, uint32_t& address, std::string& error)
		{
			Bytes body;
			std::unordered_map<uint32_t, uint32_t> where;
			const auto at = static_cast<uint32_t>(p.code.size());
			if (!Layout(a.pieces, at, body, where, error))
				return false;
			// The function starts at its first instruction (after any padding).
			address = at;
			while (address < at + body.size() && body[address - at] == op::Nop)
				++address;
			p.code.insert(p.code.end(), body.begin(), body.end());
			return true;
		}

		bool Equal(const Bytes& code, const Instruction& a, const Instruction& b)
		{
			return a.size == b.size && std::equal(code.begin() + a.at, code.begin() + a.at + a.size, code.begin() + b.at);
		}

		// ---- identity helpers ----

		// is_playable: ENTER 1; LOCAL_U8_LOAD 0; PUSH 3; ILT; LEAVE 1 1.
		constexpr uint8_t kIsPlayable[] = {45, 1, 3, 0, 0, 56, 0, 116, 12, 46, 1, 1};
		// get_cur: ENTER 0; CALL update_cur; then the saved current character (G114990.2370.539.4321); LEAVE.
		constexpr uint8_t kCurLoad[] = {97, 0x2E, 0xC1, 0x01, 70, 0x42, 0x09, 70, 0x1B, 0x02, 71, 0xE1, 0x10};

		struct Helpers
		{
			std::vector<uint32_t> getCur, isPlayable;
			std::unordered_set<uint32_t> internal; // update_cur and the functions it calls
		};

		Helpers FindHelpers(const Bytes& code, const std::vector<Instruction>& ins, const Functions& fns)
		{
			Helpers h;
			std::vector<uint32_t> todo;
			for (const uint32_t f : fns.starts)
			{
				if (f + sizeof(kIsPlayable) <= code.size() && std::equal(std::begin(kIsPlayable), std::end(kIsPlayable), code.begin() + f))
					h.isPlayable.push_back(f);
				if (code[f + 4] == 0 && f + 9 + sizeof(kCurLoad) < code.size() && code[f + 5] == op::Call &&
				    std::equal(std::begin(kCurLoad), std::end(kCurLoad), code.begin() + f + 9) && code[f + 9 + sizeof(kCurLoad)] == op::Leave)
				{
					h.getCur.push_back(f);
					todo.push_back(U24(code, f + 6));
				}
			}
			while (!todo.empty())
			{
				const uint32_t f = todo.back();
				todo.pop_back();
				if (!h.internal.insert(f).second)
					continue;
				const auto first = std::lower_bound(ins.begin(), ins.end(), f, [](const Instruction& i, uint32_t a) { return i.at < a; });
				for (auto it = first; it != ins.end() && it->at < fns.End(f); ++it)
					if (it->op == op::Call)
						todo.push_back(U24(code, it->at + 1));
			}
			return h;
		}

		// get_cur4: get_cur, or 3 when that is 145 (none) and the officer is the player.
		Asm GetCur4(uint32_t getCur, uint16_t officer)
		{
			Asm a;
			const uint32_t ret = a.NewLabel();
			a.Enter(0, 2);
			a.Call(getCur);
			a.Raw({op::Dup, op::PushU8, 145});
			a.Jump(op::IeqJz, ret);
			a.Native(officer, 0, 1);
			a.Jump(op::Jz, ret);
			a.Raw({op::Drop});
			a.Push(3);
			a.Leave(0, 1);
			a.Mark(ret);
			a.Leave(0, 1);
			return a;
		}

		// is_char4(c): c < 3, or c == 3 while the officer is the player.
		Asm IsChar4(uint16_t officer)
		{
			Asm a;
			const uint32_t notProtagonist = a.NewLabel(), no = a.NewLabel();
			a.Enter(1, 3);
			a.Local(0);
			a.Push(3);
			a.Jump(op::IltJz, notProtagonist);
			a.Push(1);
			a.Leave(1, 1);
			a.Mark(notProtagonist);
			a.Local(0);
			a.Push(3);
			a.Jump(op::IeqJz, no);
			a.Native(officer, 0, 1);
			a.Leave(1, 1);
			a.Mark(no);
			a.Push(0);
			a.Leave(1, 1);
			return a;
		}

		// "c == 0 || c == 1 || c == 2" on a local (short-circuit form of the compiler).
		bool IsChain(const Bytes& code, uint32_t s)
		{
			if (s + 24 > code.size())
				return false;
			const uint8_t n = code[s + 1];
			static constexpr uint8_t kShape[] = {56, 0, 113, 8, 42, 6, 86, 0, 0, 56, 0, 114, 8, 32, 42, 6, 86, 0, 0, 56, 0, 115, 8, 32};
			for (uint32_t i = 0; i < 24; ++i)
			{
				if (i == 7 || i == 8 || i == 17 || i == 18)
					continue;
				const uint8_t want = (i == 1 || i == 10 || i == 20) ? n : kShape[i];
				if (code[s + i] != want)
					return false;
			}
			return JumpTarget(code, s + 6) == s + 14 && JumpTarget(code, s + 16) == s + 24;
		}

		// ---- character switches ----

		// A case body: from its first instruction to its first J or LEAVE. Page padding (NOPs, and a J over them to
		// the next page) is not part of it.
		std::optional<std::vector<Instruction>> CaseBody(const Bytes& code, uint32_t target, uint32_t end)
		{
			std::vector<Instruction> out;
			for (uint32_t ip = target; ip < end && out.size() < 64;)
			{
				const Instruction i{ip, code[ip], InstructionSize(code, ip)};
				if (i.op == op::Nop)
				{
					++ip;
					continue;
				}
				if (i.op == op::J && JumpTarget(code, ip) > ip + 3 && JumpTarget(code, ip) <= end &&
				    std::all_of(code.begin() + ip + 3, code.begin() + JumpTarget(code, ip), [](uint8_t b) { return b == op::Nop; }))
				{
					ip = JumpTarget(code, ip);
					continue;
				}
				out.push_back(i);
				if (i.op == op::J || i.op == op::Leave)
					return out;
				if (i.op == op::Switch || i.op == op::Enter)
					return std::nullopt;
				ip += i.size;
			}
			return std::nullopt;
		}

		// The value a constant push instruction pushes (integer pushes of one value).
		bool PushedConstant(const Bytes& code, const Instruction& i, int32_t& value)
		{
			int small = 0;
			if (op::SmallPush(i.op, small))
				value = small;
			else if (i.op == op::PushU8)
				value = code[i.at + 1];
			else if (i.op == op::PushS16)
				value = S16(code, i.at + 1);
			else if (i.op == op::PushU24)
				value = static_cast<int32_t>(U24(code, i.at + 1));
			else if (i.op == op::PushU32)
				value = S32(code, i.at + 1);
			else
				return false;
			return true;
		}

		// How case 3 differs from the template case at one instruction.
		struct Change
		{
			enum Kind { Copy, Stat, Character } kind = Copy;
			uint32_t stat = 0; // index into CharacterStats::stats
		};

		// Compares the bodies of the cases `values` instruction by instruction. Equal instructions (jumps by target,
		// pushes by value) are copied; the others must push the case's own stat hash (SPk_X of one X) or its own value.
		// Empty when the bodies differ otherwise, or do not differ at all.
		std::optional<std::vector<Change>> Compare(const Bytes& code, const CharacterStats& stats, const std::vector<std::vector<Instruction>>& bodies,
		    const std::vector<int>& values)
		{
			const size_t n = bodies[0].size();
			for (const auto& b : bodies)
				if (b.size() != n)
					return std::nullopt;
			std::vector<Change> changes(n);
			for (size_t k = 0; k < n; ++k)
			{
				std::vector<int32_t> pushed(bodies.size());
				bool constants = true;
				for (size_t c = 0; c < bodies.size() && constants; ++c)
					constants = PushedConstant(code, bodies[c][k], pushed[c]);
				if (constants)
				{
					if (std::all_of(pushed.begin(), pushed.end(), [&](int32_t v) { return v == pushed[0]; }))
						continue;
					bool stat = true, character = true;
					uint32_t index = UINT32_MAX;
					for (size_t c = 0; c < bodies.size(); ++c)
					{
						const auto it = stats.byHash[values[c]].find(static_cast<uint32_t>(pushed[c]));
						stat = stat && it != stats.byHash[values[c]].end() && (index == UINT32_MAX || it->second == index);
						if (stat)
							index = it->second;
						character = character && pushed[c] == values[c];
					}
					if (stat)
						changes[k] = {Change::Stat, index};
					else if (character)
						changes[k] = {Change::Character};
					else
						return std::nullopt;
					continue;
				}
				const Instruction& first = bodies[0][k];
				for (size_t c = 1; c < bodies.size(); ++c)
				{
					const Instruction& other = bodies[c][k];
					const bool same = op::IsJump(first.op) ? other.op == first.op && JumpTarget(code, other.at) == JumpTarget(code, first.at)
					                                       : Equal(code, first, other);
					if (!same)
						return std::nullopt;
				}
			}
			if (std::all_of(changes.begin(), changes.end(), [](const Change& c) { return c.kind == Change::Copy; }))
				return std::nullopt;
			return changes;
		}

		struct NewCase
		{
			std::vector<Instruction> body; // the template (case 2)
			std::vector<Change> changes;
		};

		// The case-3 body as pieces: the template with SP3 hashes / 3 in place of the character's, jumps by target.
		void EmitCase(Asm& a, const Bytes& code, const CharacterStats& stats, const NewCase& c, uint32_t label)
		{
			a.Mark(label);
			for (size_t k = 0; k < c.body.size(); ++k)
			{
				const Instruction& i = c.body[k];
				switch (c.changes[k].kind)
				{
				case Change::Stat:
					a.Push(static_cast<int32_t>(convert::Joaat("SP3_" + stats.stats[c.changes[k].stat].name)));
					break;
				case Change::Character:
					a.Push(3);
					break;
				default:
					if (op::IsJump(i.op))
						a.Jump(i.op, JumpTarget(code, i.at));
					else
						a.pieces.push_back({kNoLabel, Bytes(code.begin() + i.at, code.begin() + i.at + i.size)});
				}
			}
		}

		// ---- accounts ----

		// "account == 3" where the account comes from the character sheet (field 17 of a 29-slot entry, read inline or
		// through a getter), compared right away or through a local: the PUSH 3 of each.
		std::vector<uint32_t> AccountChecks(const Bytes& code, const std::vector<Instruction>& ins, const std::unordered_set<uint32_t>& getters)
		{
			const auto compare = [&](size_t j) {
				return j < ins.size() && (ins[j].op == op::Ieq || ins[j].op == op::Ine || ins[j].op == op::IeqJz || ins[j].op == op::IneJz);
			};
			std::vector<uint32_t> out;
			for (size_t k = 1; k + 2 < ins.size(); ++k)
			{
				const Instruction& i = ins[k];
				const bool inline17 = i.op == op::IoffsetU8Load && code[i.at + 1] == 17 && ins[k - 1].op == op::ArrayU8 && code[ins[k - 1].at + 1] == 29;
				if (!inline17 && !(i.op == op::Call && getters.contains(U24(code, i.at + 1))))
					continue;
				if (ins[k + 1].op == op::Push3 && compare(k + 2))
				{
					out.push_back(ins[k + 1].at);
					continue;
				}
				if (ins[k + 1].op != op::LocalU8Store)
					continue;
				const uint8_t local = code[ins[k + 1].at + 1];
				for (size_t j = k + 2; j + 2 < ins.size() && j < k + 24; ++j)
				{
					if (ins[j].op == op::Enter || (ins[j].op == op::LocalU8Store && code[ins[j].at + 1] == local))
						break;
					if (ins[j].op == op::LocalU8Load && code[ins[j].at + 1] == local && ins[j + 1].op == op::Push3 && compare(j + 2))
						out.push_back(ins[j + 1].at);
				}
			}
			return out;
		}

		// ---- gunclub_shop: weapons the officer is never offered (§0.0 Q9) ----

		// The weapon unlock check (character, weapon): is_playable(c), the always-unlocked weapons, then the unlock stat.
		bool IsUnlockCheck(const Bytes& code, uint32_t f, const Helpers& h)
		{
			static constexpr int kShape[] = {45, 2, -1, -1, 0, 56, 0, 93, -1, -1, -1, 86, -1, -1, 56, 1, 93, -1, -1, -1, 86, -1, -1, 114, 46, 2, 1, 56, 0, 56, 1, 93};
			if (f + std::size(kShape) > code.size())
				return false;
			for (size_t i = 0; i < std::size(kShape); ++i)
				if (kShape[i] >= 0 && code[f + i] != kShape[i])
					return false;
			return std::find(h.isPlayable.begin(), h.isPlayable.end(), U24(code, f + 8)) != h.isPlayable.end();
		}

		// The always-unlocked check (weapon): a SWITCH on the weapon (the hammer is one of its cases), then every DLC
		// weapon whose content is not locked. It knows no character: the officer native decides.
		bool IsAlwaysUnlocked(const Bytes& code, uint32_t f)
		{
			if (f + 9 > code.size() || code[f] != op::Enter || code[f + 1] != 1 || code[f + 4] != 0 || code[f + 5] != op::LocalU8Load ||
			    code[f + 6] != 0 || code[f + 7] != op::Switch)
				return false;
			// Four weapons, all answered "unlocked" (PUSH 1; LEAVE 1 1) at once.
			const auto cases = SwitchCases(code, f + 7);
			const uint32_t yes = cases.empty() ? 0 : cases[0].target;
			return cases.size() == 4 && yes + 4 <= code.size() && code[yes] == op::Push0 + 1 && code[yes + 1] == op::Leave && code[yes + 2] == 1 &&
			       code[yes + 3] == 1 && std::all_of(cases.begin(), cases.end(), [yes](const SwitchCase& c) { return c.target == yes; }) &&
			       std::any_of(cases.begin(), cases.end(), [](const SwitchCase& c) { return static_cast<uint32_t>(c.value) == convert::Joaat("WEAPON_HAMMER"); });
		}

		// The shop's "is this weapon on sale" (weapon): a DLC weapon whose content is locked is not; otherwise any valid
		// weapon is (IS_WEAPON_VALID at the end). No character: the officer native decides.
		bool IsOnSale(const Bytes& code, uint32_t f, uint32_t end)
		{
			// The DLC data's weapon against the parameter, and LOCAL_U8_LOAD 0; NATIVE (1 argument, 1 result); LEAVE 1 1 last.
			static constexpr int kDlcCompare[] = {44, 9, -1, -1, 86, -1, -1, 55, -1, 65, 1, 56, 0, 87};
			if (code[f + 1] != 1 || end < f + 20)
				return false;
			uint32_t last = end;
			while (last > f && code[last - 1] == op::Nop)
				--last;
			if (last < f + 9 || code[last - 9] != op::LocalU8Load || code[last - 8] != 0 || code[last - 7] != op::Native || code[last - 6] != (1 << 2 | 1) ||
			    code[last - 3] != op::Leave || code[last - 2] != 1 || code[last - 1] != 1)
				return false;
			for (uint32_t at = f; at + std::size(kDlcCompare) <= last; ++at)
			{
				bool match = true;
				for (size_t i = 0; i < std::size(kDlcCompare) && match; ++i)
					match = kDlcCompare[i] < 0 || code[at + i] == kDlcCompare[i];
				if (match)
					return true;
			}
			return false;
		}

		// ---- player-speaker sites (research/char4-design.md §3.4, U39) ----

		// Offset of `text` in the string table (a whole string), or -1.
		int32_t StringOffset(const Bytes& strings, std::string_view text)
		{
			for (size_t at = 0; at + text.size() < strings.size(); ++at)
				if ((at == 0 || strings[at - 1] == 0) && strings[at + text.size()] == 0 &&
				    std::equal(text.begin(), text.end(), strings.begin() + at))
					return static_cast<int32_t>(at);
			return -1;
		}

		// taxilauncher: the phone booking registers the player as a speaker with the protagonist's voice and lines
		// (MICHAEL / TX_1M / TX_3M for 0, ...), none for 3, and the call then never connects. The officer gets the
		// dispatcher only: voice TaxiDispatch and the dispatcher's lines TX_2 (never a protagonist voice).
		struct TaxiVoice
		{
			uint32_t function = 0, leave = 0; // the voice function and its final LEAVE
			uint8_t character = 0;            // the local holding the current character
			uint16_t voice = 0;               // the static of the voice (the two line statics follow it)
			int16_t dispatcher = 0, line = 0; // string offsets
		};
		std::optional<TaxiVoice> FindTaxiVoice(const Program& p, const std::vector<Instruction>& ins, const Functions& fns, const Helpers& h)
		{
			const Bytes& code = p.code;
			const int32_t michael = StringOffset(p.strings, "MICHAEL"), dispatcher = StringOffset(p.strings, "TaxiDispatch"),
			              line = StringOffset(p.strings, "TX_2");
			if (michael < 0 || dispatcher < 0 || line < 0 || michael > INT16_MAX || dispatcher > INT16_MAX || line > INT16_MAX)
				return std::nullopt;
			for (size_t k = 0; k + 2 < ins.size(); ++k)
			{
				// PUSH_S16 "MICHAEL"; STRING; STATIC_U16_STORE voice
				if (ins[k].op != op::PushS16 || S16(code, ins[k].at + 1) != michael || ins[k + 1].op != 102 || ins[k + 2].op != 81)
					continue;
				TaxiVoice v;
				v.function = fns.Owner(ins[k].at);
				v.voice = static_cast<uint16_t>(code[ins[k + 2].at + 1] | code[ins[k + 2].at + 2] << 8);
				v.dispatcher = static_cast<int16_t>(dispatcher);
				v.line = static_cast<int16_t>(line);
				// ENTER; CALL get_cur; LOCAL_U8_STORE c
				const uint32_t f = v.function, body = f + 5 + code[f + 4];
				if (code[f + 1] != 0 || code[body] != op::Call || std::find(h.getCur.begin(), h.getCur.end(), U24(code, body + 1)) == h.getCur.end() ||
				    code[body + 4] != op::LocalU8Store)
					return std::nullopt;
				v.character = code[body + 5];
				for (const Instruction& i : Decode(code, f, fns.End(f)))
					if (i.op == op::Leave)
						v.leave = i.at;
				return v;
			}
			return std::nullopt;
		}

		// taxilauncher: the taxi is sent once the call ends if the conversation reached its second line (STATIC_U8_LOAD s;
		// INOT; JZ; NATIVE GET_CURRENT_SCRIPTED_CONVERSATION_LINE; PUSH 1; IEQ_JZ; PUSH 1; STATIC_U8_STORE s). The
		// officer's call has no player lines, so for them it counts as reached.
		std::vector<uint32_t> FindLineChecks(const Bytes& code)
		{
			static constexpr int kShape[] = {59, -1, 6, 86, -1, -1, 44, 1, -1, -1, 114, 87, -1, -1, 114, 60};
			std::vector<uint32_t> out;
			for (uint32_t at = 0; at + std::size(kShape) + 1 < code.size(); ++at)
			{
				bool match = true;
				for (size_t i = 0; i < std::size(kShape) && match; ++i)
					match = kShape[i] < 0 || code[at + i] == kShape[i];
				if (match && code[at + 1] == code[at + 16])
					out.push_back(at + 6);
			}
			return out;
		}
	}

	std::vector<uint32_t> ForbiddenUnlockBits(const Program& p)
	{
		// The table: ENTER 1; LOCAL_U8_LOAD 0; SWITCH on the weapon, each case "push its index; LEAVE 1 1" (the pistol is 1).
		const Bytes& code = p.code;
		const auto ins = Decode(code);
		for (const uint32_t f : Functions(ins, static_cast<uint32_t>(code.size())).starts)
		{
			if (f + 9 > code.size() || code[f + 1] != 1 || code[f + 4] != 0 || code[f + 5] != op::LocalU8Load || code[f + 6] != 0 || code[f + 7] != op::Switch)
				continue;
			const auto cases = SwitchCases(code, f + 7);
			if (cases.size() < 60)
				continue;
			std::unordered_map<uint32_t, int32_t> index;
			bool table = true;
			for (const SwitchCase& c : cases)
			{
				const Instruction push{c.target, code[c.target], InstructionSize(code, c.target)};
				int32_t v = 0;
				const uint32_t leave = c.target + push.size;
				table = table && PushedConstant(code, push, v) && v >= 0 && v < 128 && leave + 3 <= code.size() && code[leave] == op::Leave &&
				        code[leave + 1] == 1 && code[leave + 2] == 1;
				index[static_cast<uint32_t>(c.value)] = v;
			}
			// Every weapon its own index (other tables answer the same value for all).
			std::unordered_set<int32_t> distinct;
			for (const auto& [weapon, i] : index)
				distinct.insert(i);
			if (!table || distinct.size() < cases.size() * 3 / 4 || index[convert::Joaat("WEAPON_PISTOL")] != 1)
				continue;
			std::vector<uint32_t> masks(4, 0);
			for (const char* w : kForbiddenWeapons)
				if (const auto it = index.find(convert::Joaat(w)); it != index.end())
					masks[it->second / 32] |= 1u << (it->second % 32);
			return masks;
		}
		return {};
	}

	void CharacterStats::Parse(std::string_view xml)
	{
		const auto attr = [](std::string_view tag, std::string_view name) -> std::string_view {
			const std::string key = std::string(" ") + std::string(name) + "=\"";
			const size_t at = tag.find(key);
			if (at == std::string_view::npos)
				return {};
			const size_t start = at + key.size(), end = tag.find('"', start);
			return end == std::string_view::npos ? std::string_view{} : tag.substr(start, end - start);
		};
		for (size_t at = xml.find("<stat "); at != std::string_view::npos; at = xml.find("<stat ", at + 1))
		{
			const size_t end = xml.find('>', at);
			if (end == std::string_view::npos)
				break;
			const std::string_view tag = xml.substr(at, end - at);
			if (attr(tag, "characterStat") != "true")
				continue;
			const std::string_view name = attr(tag, "Name");
			if (name.empty())
				continue;
			const auto index = static_cast<uint32_t>(stats.size());
			stats.push_back({std::string(name), std::string(attr(tag, "Type"))});
			for (int c = 0; c < 3; ++c)
				byHash[c].emplace(convert::Joaat(std::format("SP{}_{}", c, name)), index);
		}
	}

	const CharacterStats::Stat* CharacterStats::Find(int character, uint32_t hash) const
	{
		const auto it = byHash[character].find(hash);
		return it == byHash[character].end() ? nullptr : &stats[it->second];
	}

	bool Rewrite(Program& p, const CharacterStats& stats, Report& report, std::string& error)
	{
		Bytes& code = p.code;
		const auto originalSize = static_cast<uint32_t>(code.size());
		auto ins = Decode(code);
		const Functions fns(ins, originalSize);
		const Helpers h = FindHelpers(code, ins, fns);
		if (h.getCur.empty() || h.isPlayable.empty())
		{
			error = std::format("identity helpers not found (get_cur {}, is_playable {})", h.getCur.size(), h.isPlayable.size());
			return false;
		}
		const uint16_t officer = p.NativeIndex(kOfficerNative);
		std::string name = p.name; // as the script calls itself (e.g. "taxiLauncher")
		std::transform(name.begin(), name.end(), name.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });

		// Character switches and the functions to re-emit (before anything is appended).
		std::map<uint32_t, std::map<uint32_t, NewCase>> extend; // function -> SWITCH -> its case 3
		for (const Instruction& sw : ins)
		{
			if (sw.op != op::Switch)
				continue;
			const auto cases = SwitchCases(code, sw.at);
			std::vector<int> values;
			for (const auto& c : cases)
				values.push_back(c.value);
			std::sort(values.begin(), values.end());
			if (values != std::vector<int>{0, 1, 2})
				continue;
			const uint32_t f = fns.Owner(sw.at), end = fns.End(f);
			std::vector<std::vector<Instruction>> bodies(3);
			bool ok = true;
			for (const auto& c : cases)
			{
				auto b = CaseBody(code, c.target, end);
				ok = ok && b.has_value();
				if (ok)
					bodies[c.value] = std::move(*b);
			}
			if (!ok)
				continue;
			bool hasStat = false;
			for (int c = 0; c < 3; ++c)
				for (const Instruction& i : bodies[c])
				{
					int32_t v = 0;
					hasStat = hasStat || (PushedConstant(code, i, v) && stats.Find(c, static_cast<uint32_t>(v)));
				}
			if (auto changes = Compare(code, stats, bodies, {0, 1, 2}))
			{
				const bool statKind = std::any_of(changes->begin(), changes->end(), [](const Change& c) { return c.kind == Change::Stat; });
				(statKind ? report.statSwitches : report.identitySwitches)++;
				for (const Change& c : *changes)
					if (c.kind == Change::Stat)
						report.stats.insert(stats.stats[c.stat].name);
				extend[f][sw.at] = {bodies[2], std::move(*changes)};
				continue;
			}
			// Loose form: Michael's case does more; Franklin's and Trevor's agree, and Michael's pushes his stat.
			if (auto changes = Compare(code, stats, {bodies[1], bodies[2]}, {1, 2}))
			{
				bool michael = true;
				for (const Change& c : *changes)
				{
					if (c.kind == Change::Character)
						michael = false;
					if (c.kind != Change::Stat)
						continue;
					const uint32_t want = convert::Joaat("SP0_" + stats.stats[c.stat].name);
					michael = michael && std::any_of(bodies[0].begin(), bodies[0].end(), [&](const Instruction& i) {
						int32_t v = 0;
						return PushedConstant(code, i, v) && static_cast<uint32_t>(v) == want;
					});
				}
				if (michael)
				{
					report.looseSwitches++;
					for (const Change& c : *changes)
						if (c.kind == Change::Stat)
							report.stats.insert(stats.stats[c.stat].name);
					extend[f][sw.at] = {bodies[2], std::move(*changes)};
					continue;
				}
			}
			if (hasStat)
				report.notes.push_back(std::format("SWITCH @{} (in @{}) on 0/1/2 with stat hashes left alone", sw.at, f));
		}

		// Inline playable chains, account checks and the script rules, all in place.
		std::vector<uint32_t> chains;
		for (const Instruction& i : ins)
			if (i.op == op::LocalU8Load && IsChain(code, i.at))
				chains.push_back(i.at);
		std::unordered_set<uint32_t> accountGetters;
		for (const uint32_t f : fns.starts)
		{
			const uint32_t b = f + 5 + code[f + 4];
			if (code[f + 1] != 1 || b + 11 > code.size() || code[b] != op::LocalU8Load || code[b + 1] != 0)
				continue;
			const bool runtime = code[b + 2] == op::GlobalU16 && code[b + 3] == (2349 & 0xFF) && code[b + 4] == (2349 >> 8);
			const bool saved = code[b + 2] == op::GlobalU24 && U24(code, b + 3) == 143050;
			const uint32_t r = b + (runtime ? 5 : 6);
			if ((runtime || saved) && code[r] == op::ArrayU8 && code[r + 1] == 29 && code[r + 2] == op::IoffsetU8Load && code[r + 3] == 17 &&
			    code[r + 4] == op::Leave)
				accountGetters.insert(f);
		}
		const std::vector<uint32_t> accountChecks = AccountChecks(code, ins, accountGetters);
		std::optional<TaxiVoice> taxiVoice;
		std::vector<uint32_t> lineChecks;
		if (name == "taxilauncher")
		{
			taxiVoice = FindTaxiVoice(p, ins, fns, h);
			lineChecks = FindLineChecks(code);
			if (!taxiVoice || lineChecks.size() != 1)
				report.notes.push_back("taxilauncher: the phone booking's speaker or line check not found; the officer's taxi calls do not connect");
		}
		// gunclub_shop: the checks that refuse the forbidden weapons; the weapon parameter, and the character's (or -1:
		// the officer native).
		struct Refusal
		{
			uint8_t weapon;
			int character;
			uint32_t forbidden = 0; // the forbidden-weapon function, once appended
		};
		std::map<uint32_t, Refusal> refusals;
		if (name == "gunclub_shop")
			for (const uint32_t f : fns.starts)
				if (IsUnlockCheck(code, f, h))
					refusals[f] = {1, 0};
				else if (IsAlwaysUnlocked(code, f) || IsOnSale(code, f, fns.End(f)))
					refusals[f] = {0, -1};
		// The shop's "is this item unlocked" (weapon, ...): DLC weapons are, before it asks the unlock check for the
		// current character (CALL get_cur; LOCAL_U8_LOAD 0; CALL the unlock check).
		if (name == "gunclub_shop")
			for (const Instruction& i : ins)
			{
				if (i.op != op::Call || i.at + 10 > code.size() || std::find(h.getCur.begin(), h.getCur.end(), U24(code, i.at + 1)) == h.getCur.end() ||
				    code[i.at + 4] != op::LocalU8Load || code[i.at + 5] != 0 || code[i.at + 6] != op::Call)
					continue;
				const auto unlock = refusals.find(U24(code, i.at + 7));
				const uint32_t f = fns.Owner(i.at);
				if (unlock != refusals.end() && unlock->second.character >= 0 && !refusals.contains(f) && code[f + 1] >= 1)
					refusals[f] = {0, -1};
			}

		// Appended helpers.
		std::vector<uint32_t> getCur4(h.getCur.size());
		for (size_t k = 0; k < h.getCur.size(); ++k)
			if (!Append(p, GetCur4(h.getCur[k], officer), getCur4[k], error))
				return false;
		uint32_t isChar4 = 0;
		if (!Append(p, IsChar4(officer), isChar4, error))
			return false;

		// Calls of the identity helpers (not inside update_cur and its helpers).
		for (const Instruction& i : ins)
		{
			if (i.op != op::Call || h.internal.contains(fns.Owner(i.at)))
				continue;
			const uint32_t target = U24(code, i.at + 1);
			uint32_t to = 0;
			if (const auto it = std::find(h.getCur.begin(), h.getCur.end(), target); it != h.getCur.end())
				to = getCur4[it - h.getCur.begin()], report.curCalls++;
			else if (std::find(h.isPlayable.begin(), h.isPlayable.end(), target) != h.isPlayable.end())
				to = isChar4, report.playableCalls++;
			else
				continue;
			code[i.at + 1] = uint8_t(to);
			code[i.at + 2] = uint8_t(to >> 8);
			code[i.at + 3] = uint8_t(to >> 16);
		}
		for (const uint32_t at : accountChecks)
			code[at] = op::Push4;
		report.accountChecks = static_cast<int>(accountChecks.size());
		for (const uint32_t s : chains)
		{
			// Nothing else may jump into the chain; the call must not cross a code page.
			const uint32_t f = fns.Owner(s);
			bool entered = false;
			for (const Instruction& i : Decode(code, f, fns.End(f)))
				if (op::IsJump(i.op) && i.at != s + 6 && i.at != s + 16)
					entered = entered || (JumpTarget(code, i.at) > s && JumpTarget(code, i.at) < s + 24);
			if (entered || s / kCodePage != (s + 5) / kCodePage)
			{
				report.notes.push_back(std::format("playable chain @{} left alone", s));
				continue;
			}
			const uint8_t n = code[s + 1];
			const uint8_t call[] = {op::LocalU8Load, n, op::Call, uint8_t(isChar4), uint8_t(isChar4 >> 8), uint8_t(isChar4 >> 16)};
			std::copy(std::begin(call), std::end(call), code.begin() + s);
			std::fill(code.begin() + s + 6, code.begin() + s + 24, op::Nop);
			report.chains++;
		}

		// taxilauncher: the conversation line check answers 1 for the officer.
		for (const uint32_t at : lineChecks)
		{
			Asm a;
			const uint32_t original = a.NewLabel();
			a.Enter(0, 2);
			a.Native(officer, 0, 1);
			a.Jump(op::Jz, original);
			a.Push(1);
			a.Leave(0, 1);
			a.Mark(original);
			a.pieces.push_back({kNoLabel, Bytes(code.begin() + at, code.begin() + at + 4)});
			a.Leave(0, 1);
			uint32_t helper = 0;
			if (!Append(p, a, helper, error))
				return false;
			const uint8_t call[] = {op::Call, uint8_t(helper), uint8_t(helper >> 8), uint8_t(helper >> 16)};
			std::copy(std::begin(call), std::end(call), code.begin() + at);
		}

		// gunclub_shop: the checks refuse the forbidden weapons to character 3 first.
		if (name == "gunclub_shop")
		{
			uint32_t forbidden = 0;
			for (auto& [f, refusal] : refusals)
				{
					if (!forbidden)
					{
						Asm a;
						const uint32_t yes = a.NewLabel();
						a.Enter(1, 3);
						a.Local(0);
						Piece sw{kNoLabel, Bytes{op::Switch, uint8_t(std::size(kForbiddenWeapons))}};
						for (const char* w : kForbiddenWeapons)
						{
							const uint32_t hash = convert::Joaat(w);
							sw.rel.push_back({static_cast<uint32_t>(sw.bytes.size() + 4), yes});
							sw.bytes.insert(sw.bytes.end(), {uint8_t(hash), uint8_t(hash >> 8), uint8_t(hash >> 16), uint8_t(hash >> 24), 0, 0});
						}
						a.pieces.push_back(std::move(sw));
						a.Push(0);
						a.Leave(1, 1);
						a.Mark(yes);
						a.Push(1);
						a.Leave(1, 1);
						if (!Append(p, a, forbidden, error))
							return false;
					}
					refusal.forbidden = forbidden;
				}
			if (std::count_if(refusals.begin(), refusals.end(), [](const auto& r) { return r.second.character >= 0; }) != 1 ||
			    std::count_if(refusals.begin(), refusals.end(), [](const auto& r) { return r.second.character < 0; }) != 3)
				report.notes.push_back(std::format("gunclub_shop: {} weapon check(s) found instead of 4; the officer may be offered forbidden weapons",
				    refusals.size()));
		}

		// Re-emit the functions that change size (in address order), each forwarding from its old place.
		std::set<uint32_t> functions;
		for (const auto& [f, _] : extend)
			functions.insert(f);
		for (const auto& [f, _] : refusals)
			functions.insert(f);
		if (taxiVoice && lineChecks.size() == 1)
			functions.insert(taxiVoice->function);
		for (const uint32_t f : functions)
		{
			const uint32_t end = fns.End(f);
			std::vector<Piece> pieces = Pieces(code, f, end);
			Asm extra;
			extra.next = kNewLabels;
			if (const auto it = refusals.find(f); it != refusals.end())
			{
				// After ENTER: if ((c == 3, or the officer is the player) && forbidden(w)) return 0;
				Asm a;
				const uint32_t skip = pieces[1].label;
				if (it->second.character >= 0)
				{
					a.Local(static_cast<uint8_t>(it->second.character));
					a.Push(3);
					a.Jump(op::IeqJz, skip);
				}
				else
				{
					a.Native(officer, 0, 1);
					a.Jump(op::Jz, skip);
				}
				a.Local(it->second.weapon);
				a.Call(it->second.forbidden);
				a.Jump(op::Jz, skip);
				a.Push(0);
				a.Leave(code[f + 1], 1);
				pieces.insert(pieces.begin() + 1, a.pieces.begin(), a.pieces.end());
			}
			if (taxiVoice && taxiVoice->function == f)
			{
				// Before the final LEAVE: if (c == 3) { voice = "TaxiDispatch"; line1 = line3 = "TX_2"; }
				Asm a;
				const auto leave = std::find_if(pieces.begin(), pieces.end(), [&](const Piece& x) { return x.label == taxiVoice->leave; });
				const uint32_t skip = leave->label;
				a.Local(taxiVoice->character);
				a.Push(3);
				a.Jump(op::IeqJz, skip);
				const auto store = [&](int16_t text, uint16_t slot) {
					a.Raw({op::PushS16, uint8_t(text), uint8_t(uint16_t(text) >> 8), 102, 81, uint8_t(slot), uint8_t(slot >> 8)});
				};
				store(taxiVoice->dispatcher, taxiVoice->voice);
				store(taxiVoice->line, static_cast<uint16_t>(taxiVoice->voice + 1));
				store(taxiVoice->line, static_cast<uint16_t>(taxiVoice->voice + 2));
				pieces.insert(leave, a.pieces.begin(), a.pieces.end());
			}
			if (const auto it = extend.find(f); it != extend.end())
				for (auto& [at, nc] : it->second)
				{
					const uint32_t label = extra.NewLabel();
					auto piece = std::find_if(pieces.begin(), pieces.end(), [at](const Piece& x) { return x.label == at; });
					piece->bytes[1]++;
					piece->rel.push_back({static_cast<uint32_t>(piece->bytes.size() + 4), label});
					piece->bytes.insert(piece->bytes.end(), {3, 0, 0, 0, 0, 0});
					EmitCase(extra, code, stats, nc, label);
				}
			pieces.insert(pieces.end(), extra.pieces.begin(), extra.pieces.end());
			if (!Relocate(p, f, end, pieces, error))
				return false;
			report.moved++;
		}
		return true;
	}
}
