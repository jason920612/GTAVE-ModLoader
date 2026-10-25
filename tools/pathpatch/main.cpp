// pathpatch: the code patches that let the game's road network (path nodes) hold positions relative to their region
// (research/phase0.md §61). Node and junction x/y stay int16 in 0.25 m units, but each region adds its own origin, so
// regions can lie anywhere in the world box instead of only within +-8192 m.
//
// Every instruction that reads a node's or junction's x/y jumps to a stub that adds the origin of the record's region,
// from a table the loader fills (65536 entries indexed by the 16-bit area id, zero for ids no region uses):
//   scalar   movsx r, word [n + 0x1c|0x1e]          -> the same, then r += origin (units)
//   vector   movd x, [n + 0x1c]; pmovsxwd y, x       -> the same, then y += (origin x, origin y, 0, 0)
//   lanes    movd/movzx+movd, pinsrw..., pmovsxwd    -> the same, then each lane += its own node's origin
//   pmov     pmovsxwd y, x (the pair loaded earlier) -> the same, then y += origin (area from a pointer chain)
//   cmp16    cmp word [n + 0x1e], q16                -> cmp (n.y + origin y), q32 (the query's full 32 bits)
// The area id is the node's own (+0x10, next to the field read), or follows a pointer chain given in the overrides
// (junctions have none of their own). The stubs only use mov/lea/movzx/paddd/pextrd/pinsrd/push/pop: the flags are
// never touched. Instructions the 5-byte jump covers are moved into the stub (rip-relative operands and relative
// branches relocated); a patch is refused if any branch of the function lands inside its bytes.
//
// usage: pathpatch <dump.exe> <sites.tsv> <out.inc> [overrides.tsv [grid.tsv]]
//   sites.tsv:     rva  record(node|junction)  field(x|y)  source  hits  class
//   overrides.tsv: rva  what [what...]
//     skip                      not a node read
//     area:<reg>:<o1>,...,<on>  area id = word at [...[[reg + o1] + o2]... + on] (qword loads, the last a word)
//     cmp16-q16                 compare against the sign-extended 16-bit query (its upper half is not its sign)
//     movsx                     a movzx read: load it sign-extended and add the origin (32-bit absolute value)
//     pmov                      a pmovsxwd decoding a pair loaded earlier (needs area:)
//     rewrite:<hex>             replace the instruction's bytes in place (nop-padded), no stub
//   grid.tsv:      rva [imm] -- reads of the region grid's constants (8192, 1/512, 512, 256, -8192, in any lane of a
//                  vector), pointed at copies scaled for 1024 m regions from -16384 (still 32 x 32); "imm": a float
//                  immediate of the instruction, rewritten scaled
#include <Zydis/Zydis.h>

#include <algorithm>
#include <array>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <map>
#include <set>
#include <sstream>
#include <string>
#include <vector>

#include "../relocate/image.hpp"

namespace
{
	// The stub block the loader allocates near the game: the two origin tables, then the stubs.
	constexpr uint32_t kTable16 = 0;         // 65536 x 16 B: (x, y, 0, 0) int32, for paddd
	constexpr uint32_t kTable8 = 65536 * 16; // 65536 x 8 B: (x, y) int32
	constexpr uint32_t kStubs = kTable8 + 65536 * 8;

	struct Site
	{
		uint32_t rva = 0;
		std::string record, field, source, cls;
	};

	struct Reloc
	{
		uint32_t at;     // offset of the rel32 / disp32 field in the stub block
		uint32_t target; // game RVA it refers to
		uint32_t end;    // offset of the end of its instruction (the rel32 is relative to it)
	};

	struct Patch
	{
		uint32_t rva = 0, length = 0; // the bytes overwritten with jmp rel32 (+ nops)
		std::vector<uint8_t> original;
		uint32_t stub = 0;            // offset in the stub block
	};

	struct Rewrite
	{
		uint32_t rva = 0;
		std::vector<uint8_t> original, replacement;
	};

	struct Decoded
	{
		ZydisDecodedInstruction instr{};
		ZydisDecodedOperand ops[ZYDIS_MAX_OPERAND_COUNT]{};
		uint32_t rva = 0;
	};

	ZydisDecoder g_decoder;
	const relocate::Image* g_image = nullptr;

	bool Decode(uint32_t rva, Decoded& out)
	{
		const uint8_t* p = g_image->At(rva, 15);
		if (!p)
			return false;
		out.rva = rva;
		return ZYAN_SUCCESS(ZydisDecoderDecodeFull(&g_decoder, p, 15, &out.instr, out.ops));
	}

	std::string Text(const Decoded& d)
	{
		char buf[256];
		ZydisFormatter f;
		ZydisFormatterInit(&f, ZYDIS_FORMATTER_STYLE_INTEL);
		ZydisFormatterFormatInstruction(&f, &d.instr, d.ops, d.instr.operand_count_visible, buf, sizeof(buf), d.rva, nullptr);
		return buf;
	}

	ZydisRegister Widest(ZydisRegister r)
	{
		return ZydisRegisterGetLargestEnclosing(ZYDIS_MACHINE_MODE_LONG_64, r);
	}

	const ZydisRegister kScratch[] = {ZYDIS_REGISTER_RAX, ZYDIS_REGISTER_RCX, ZYDIS_REGISTER_RDX, ZYDIS_REGISTER_R8, ZYDIS_REGISTER_R9,
	    ZYDIS_REGISTER_R10, ZYDIS_REGISTER_R11, ZYDIS_REGISTER_RBX, ZYDIS_REGISTER_RSI, ZYDIS_REGISTER_RDI, ZYDIS_REGISTER_R12,
	    ZYDIS_REGISTER_R13, ZYDIS_REGISTER_R14, ZYDIS_REGISTER_R15, ZYDIS_REGISTER_RBP};

	ZydisRegister ByName(const std::string& name)
	{
		if (name == "rsp")
			return ZYDIS_REGISTER_RSP;
		for (const ZydisRegister c : kScratch)
			if (name == ZydisRegisterGetString(c))
				return c;
		return ZYDIS_REGISTER_NONE;
	}

	ZydisRegister To32(ZydisRegister r)
	{
		return ZydisRegisterEncode(ZYDIS_REGCLASS_GPR32, ZydisRegisterGetId(Widest(r)));
	}

	// Every general register an instruction touches (operands, memory base/index).
	void Uses(const Decoded& d, std::set<ZydisRegister>& out)
	{
		for (int i = 0; i < d.instr.operand_count; ++i)
		{
			const ZydisDecodedOperand& o = d.ops[i];
			if (o.type == ZYDIS_OPERAND_TYPE_REGISTER)
				out.insert(Widest(o.reg.value));
			else if (o.type == ZYDIS_OPERAND_TYPE_MEMORY)
			{
				if (o.mem.base != ZYDIS_REGISTER_NONE)
					out.insert(Widest(o.mem.base));
				if (o.mem.index != ZYDIS_REGISTER_NONE)
					out.insert(Widest(o.mem.index));
			}
		}
	}

	const ZydisDecodedOperand* MemOf(const Decoded& d)
	{
		for (int i = 0; i < d.instr.operand_count_visible; ++i)
			if (d.ops[i].type == ZYDIS_OPERAND_TYPE_MEMORY)
				return &d.ops[i];
		return nullptr;
	}

	class Emitter
	{
	public:
		std::vector<uint8_t> bytes; // the stub block after kStubs
		std::vector<Reloc> relocs;
		std::string error;

		uint32_t Here() const { return kStubs + static_cast<uint32_t>(bytes.size()); }

		bool Encode(ZydisEncoderRequest& req)
		{
			uint8_t buf[ZYDIS_MAX_INSTRUCTION_LENGTH];
			ZyanUSize len = sizeof(buf);
			if (!ZYAN_SUCCESS(ZydisEncoderEncodeInstruction(&req, buf, &len)))
			{
				error = std::string("encode failed: ") + ZydisMnemonicGetString(req.mnemonic);
				return false;
			}
			bytes.insert(bytes.end(), buf, buf + len);
			return true;
		}

		static ZydisEncoderRequest Req(ZydisMnemonic m)
		{
			ZydisEncoderRequest r{};
			r.machine_mode = ZYDIS_MACHINE_MODE_LONG_64;
			r.mnemonic = m;
			return r;
		}
		static void Reg(ZydisEncoderRequest& r, ZydisRegister reg)
		{
			auto& o = r.operands[r.operand_count++];
			o.type = ZYDIS_OPERAND_TYPE_REGISTER;
			o.reg.value = reg;
		}
		static void Imm(ZydisEncoderRequest& r, uint64_t v)
		{
			auto& o = r.operands[r.operand_count++];
			o.type = ZYDIS_OPERAND_TYPE_IMMEDIATE;
			o.imm.u = v;
		}
		static void Mem(ZydisEncoderRequest& r, ZydisRegister base, ZydisRegister index, uint8_t scale, int64_t disp, uint16_t size)
		{
			auto& o = r.operands[r.operand_count++];
			o.type = ZYDIS_OPERAND_TYPE_MEMORY;
			o.mem.base = base;
			o.mem.index = index;
			o.mem.scale = index == ZYDIS_REGISTER_NONE ? 0 : scale;
			o.mem.displacement = disp;
			o.mem.size = size;
		}

		bool Op(ZydisMnemonic m, ZydisRegister a)
		{
			auto q = Req(m);
			Reg(q, a);
			return Encode(q);
		}
		bool RegReg(ZydisMnemonic m, ZydisRegister a, ZydisRegister b)
		{
			auto q = Req(m);
			Reg(q, a);
			Reg(q, b);
			return Encode(q);
		}
		bool RegMem(ZydisMnemonic m, ZydisRegister a, ZydisRegister base, ZydisRegister index, uint8_t scale, int64_t disp, uint16_t size)
		{
			auto q = Req(m);
			Reg(q, a);
			Mem(q, base, index, scale, disp, size);
			return Encode(q);
		}
		// lea reg, [rip + target offset in the stub block]
		bool LeaTable(ZydisRegister reg, uint32_t table)
		{
			const uint32_t end = Here() + 7; // lea r64, [rip + disp32] is 7 bytes
			const size_t before = bytes.size();
			if (!RegMem(ZYDIS_MNEMONIC_LEA, reg, ZYDIS_REGISTER_RIP, ZYDIS_REGISTER_NONE, 0, static_cast<int64_t>(table) - end, 8))
				return false;
			if (bytes.size() - before != 7)
			{
				error = "lea rip is not 7 bytes";
				return false;
			}
			return true;
		}
		// dst32 = the origin's x or y (int32, units) of the area id in area32
		bool Origin(ZydisRegister dst, ZydisRegister area, bool y)
		{
			return LeaTable(dst, kTable8) && RegMem(ZYDIS_MNEMONIC_MOV, To32(dst), dst, area, 8, y ? 4 : 0, 4);
		}

		// An original instruction copied into the stub; rip-relative operands and relative branches are relocated.
		bool Relocated(const Decoded& d)
		{
			const uint8_t* src = g_image->At(d.rva, d.instr.length);
			if (d.instr.attributes & ZYDIS_ATTRIB_IS_RELATIVE)
			{
				if (d.instr.meta.category == ZYDIS_CATEGORY_COND_BR || d.instr.mnemonic == ZYDIS_MNEMONIC_JMP ||
				    d.instr.mnemonic == ZYDIS_MNEMONIC_CALL)
				{
					uint64_t target = 0;
					ZydisCalcAbsoluteAddress(&d.instr, &d.ops[0], d.rva, &target);
					if (d.instr.mnemonic == ZYDIS_MNEMONIC_JMP)
						bytes.push_back(0xE9);
					else if (d.instr.mnemonic == ZYDIS_MNEMONIC_CALL)
						bytes.push_back(0xE8);
					else
					{
						const uint8_t op = src[d.instr.raw.prefix_count];
						if (op != 0x0F && (op & 0xF0) != 0x70)
						{
							error = "relative branch that is not jcc/jmp/call: " + Text(d);
							return false;
						}
						const uint8_t cc = op == 0x0F ? src[d.instr.raw.prefix_count + 1] & 0x0F : op & 0x0F;
						bytes.push_back(0x0F);
						bytes.push_back(static_cast<uint8_t>(0x80 | cc));
					}
					relocs.push_back({Here(), static_cast<uint32_t>(target), Here() + 4});
					bytes.insert(bytes.end(), 4, 0);
					return true;
				}
				if (d.instr.raw.disp.size != 32)
				{
					error = "rip-relative without disp32: " + Text(d);
					return false;
				}
				uint64_t target = 0;
				for (int i = 0; i < d.instr.operand_count; ++i)
					if (d.ops[i].type == ZYDIS_OPERAND_TYPE_MEMORY && d.ops[i].mem.base == ZYDIS_REGISTER_RIP)
						ZydisCalcAbsoluteAddress(&d.instr, &d.ops[i], d.rva, &target);
				const uint32_t start = Here();
				bytes.insert(bytes.end(), src, src + d.instr.length);
				relocs.push_back({start + d.instr.raw.disp.offset, static_cast<uint32_t>(target), start + d.instr.length});
				return true;
			}
			bytes.insert(bytes.end(), src, src + d.instr.length);
			return true;
		}

		bool JumpBack(uint32_t rva)
		{
			bytes.push_back(0xE9);
			relocs.push_back({Here(), rva, Here() + 4});
			bytes.insert(bytes.end(), 4, 0);
			return true;
		}
	};

	// Relative branch targets inside a function (to keep jumps out of the bytes we overwrite).
	std::set<uint32_t> BranchTargets(uint32_t begin, uint32_t end)
	{
		std::set<uint32_t> out;
		for (uint32_t rva = begin; rva < end;)
		{
			Decoded d;
			if (!Decode(rva, d))
			{
				++rva;
				continue;
			}
			if ((d.instr.attributes & ZYDIS_ATTRIB_IS_RELATIVE) && d.ops[0].type == ZYDIS_OPERAND_TYPE_IMMEDIATE)
			{
				uint64_t t = 0;
				if (ZYAN_SUCCESS(ZydisCalcAbsoluteAddress(&d.instr, &d.ops[0], rva, &t)))
					out.insert(static_cast<uint32_t>(t));
			}
			rva += d.instr.length;
		}
		return out;
	}

	struct Overrides
	{
		bool skip = false, q16 = false, movsx = false, pmov = false;
		ZydisRegister chainReg = ZYDIS_REGISTER_NONE;
		std::vector<int64_t> chain; // area:<reg>:<offsets>
		std::vector<uint8_t> rewrite;
		bool bad = false;
		std::string why;
	};

	Overrides ParseOverrides(const std::string& text)
	{
		Overrides o;
		std::istringstream ss(text);
		for (std::string w; ss >> w;)
		{
			if (w == "skip")
				o.skip = true;
			else if (w == "cmp16-q16")
				o.q16 = true;
			else if (w == "movsx")
				o.movsx = true;
			else if (w == "pmov")
				o.pmov = true;
			else if (w.rfind("area:", 0) == 0)
			{
				const size_t c = w.find(':', 5);
				o.chainReg = ByName(w.substr(5, c - 5));
				std::istringstream offs(w.substr(c + 1));
				for (std::string v; std::getline(offs, v, ',');)
					o.chain.push_back(std::stoll(v, nullptr, 0));
				if (o.chainReg == ZYDIS_REGISTER_NONE || o.chain.empty())
					o.bad = true, o.why = "bad area override " + w;
			}
			else if (w.rfind("rewrite:", 0) == 0)
			{
				const std::string hex = w.substr(8);
				for (size_t i = 0; i + 1 < hex.size(); i += 2)
					o.rewrite.push_back(static_cast<uint8_t>(std::stoul(hex.substr(i, 2), nullptr, 16)));
			}
			else
				o.bad = true, o.why = "unknown override " + w;
		}
		return o;
	}

	enum class Kind
	{
		Scalar,
		Vector,
		Lanes,
		Pmov,
		Cmp16,
		Bad
	};

	struct NodeRef // a node record named by a memory operand reading one of its fields
	{
		ZydisRegister base = ZYDIS_REGISTER_NONE, index = ZYDIS_REGISTER_NONE;
		uint8_t scale = 0;
		int64_t areaDisp = 0; // displacement of the node's area id (+0x10) with the same base/index
		bool operator==(const NodeRef&) const = default;
	};

	struct Lane
	{
		int node; // index into Plan::nodes
		bool y;
	};

	struct Plan
	{
		Kind kind = Kind::Bad;
		std::string why;
		std::vector<Decoded> seq; // the instructions the template consumes (the site's first)
		std::vector<NodeRef> nodes;
		std::vector<Lane> lanes;  // lanes: one per lane of the pmovsxwd's result
		ZydisRegister out = ZYDIS_REGISTER_NONE;
		bool y = false;
		Overrides o;
	};

	// The node a read belongs to: its area id is 0x10 past the node's start, the field 0x1c (x) or 0x1e (y).
	NodeRef RefOf(const ZydisDecodedOperand& m, bool y)
	{
		return {m.mem.base, m.mem.index, m.mem.scale, m.mem.disp.value - (y ? 0xE : 0xC)};
	}

	bool PlainMemory(const ZydisDecodedOperand* m)
	{
		return m && m->mem.base != ZYDIS_REGISTER_RIP && m->mem.base != ZYDIS_REGISTER_RSP && m->mem.index != ZYDIS_REGISTER_RSP &&
		       m->mem.segment != ZYDIS_REGISTER_FS && m->mem.segment != ZYDIS_REGISTER_GS;
	}

	// What template the instruction(s) at a site take.
	Plan Classify(const Site& s, const Overrides& o, uint32_t rva)
	{
		Plan p;
		p.o = o;
		p.y = s.field == "y";
		Decoded d;
		if (!Decode(rva, d))
		{
			p.why = "undecodable";
			return p;
		}
		p.seq.push_back(d);
		if (o.bad)
		{
			p.why = o.why;
			return p;
		}
		if (s.record == "junction" && o.chainReg == ZYDIS_REGISTER_NONE)
		{
			p.why = "a junction needs area: " + Text(d);
			return p;
		}
		const auto* m = MemOf(d);
		const auto mn = d.instr.mnemonic;
		if (o.pmov)
		{
			if (mn != ZYDIS_MNEMONIC_PMOVSXWD || o.chainReg == ZYDIS_REGISTER_NONE)
				p.why = "pmov needs a pmovsxwd and area: " + Text(d);
			else
			{
				p.kind = Kind::Pmov;
				p.out = d.ops[0].reg.value;
			}
			return p;
		}
		if (!PlainMemory(m))
		{
			p.why = "no plain memory operand: " + Text(d);
			return p;
		}
		if ((mn == ZYDIS_MNEMONIC_MOVSX || (mn == ZYDIS_MNEMONIC_MOVZX && o.movsx)) && d.ops[0].type == ZYDIS_OPERAND_TYPE_REGISTER &&
		    m->size == 16)
		{
			p.kind = Kind::Scalar;
			p.nodes.push_back(RefOf(*m, p.y));
			return p;
		}
		if (mn == ZYDIS_MNEMONIC_CMP && d.ops[0].type == ZYDIS_OPERAND_TYPE_MEMORY && d.ops[1].type == ZYDIS_OPERAND_TYPE_REGISTER &&
		    m->size == 16 && p.y)
		{
			p.kind = Kind::Cmp16;
			p.nodes.push_back(RefOf(*m, true));
			return p;
		}
		// movd x, [n + 0x1c] or movzx r, [n + f]; movd x, r -- then pinsrw x, [m + g], k ... -- then pmovsxwd y, x
		ZydisRegister vec = ZYDIS_REGISTER_NONE;
		uint32_t at = rva + d.instr.length;
		std::map<int, std::pair<NodeRef, bool>> lanes;
		if (mn == ZYDIS_MNEMONIC_MOVD && d.ops[0].type == ZYDIS_OPERAND_TYPE_REGISTER && m->size == 32)
		{
			vec = d.ops[0].reg.value;
			// x then y of the same node (the read starts at its x)
			const NodeRef r = RefOf(*m, false);
			lanes[0] = {r, false};
			lanes[1] = {r, true};
		}
		else if (mn == ZYDIS_MNEMONIC_MOVZX && d.ops[0].type == ZYDIS_OPERAND_TYPE_REGISTER && m->size == 16)
		{
			Decoded mv;
			if (!Decode(at, mv) || mv.instr.mnemonic != ZYDIS_MNEMONIC_MOVD || mv.ops[1].type != ZYDIS_OPERAND_TYPE_REGISTER ||
			    Widest(mv.ops[1].reg.value) != Widest(d.ops[0].reg.value))
			{
				p.why = "special: " + Text(d);
				return p;
			}
			p.seq.push_back(mv);
			at += mv.instr.length;
			vec = mv.ops[0].reg.value;
			lanes[0] = {RefOf(*m, p.y), p.y};
		}
		else
		{
			p.why = "special: " + Text(d);
			return p;
		}
		for (int guard = 0; guard < 6; ++guard)
		{
			Decoded n;
			if (!Decode(at, n))
				break;
			if (n.instr.mnemonic == ZYDIS_MNEMONIC_PINSRW && n.ops[0].reg.value == vec)
			{
				const auto* nm = MemOf(n);
				if (!PlainMemory(nm))
				{
					p.why = "pinsrw without plain memory: " + Text(n);
					return p;
				}
				const int64_t disp = nm->mem.disp.value;
				const bool ny = (disp & 0xFF) == 0x1E || ((disp & 0xFF) != 0x1C && (disp & 2));
				lanes[static_cast<int>(n.ops[2].imm.value.u)] = {RefOf(*nm, ny), ny};
				p.seq.push_back(n);
				at += n.instr.length;
				continue;
			}
			if (n.instr.mnemonic == ZYDIS_MNEMONIC_PMOVSXWD && n.ops[1].type == ZYDIS_OPERAND_TYPE_REGISTER && n.ops[1].reg.value == vec)
			{
				p.seq.push_back(n);
				p.out = n.ops[0].reg.value;
				break;
			}
			// an unrelated instruction scheduled in between: fine if it only writes another xmm register (it moves along)
			bool harmless = n.instr.operand_count_visible >= 1 && n.ops[0].type == ZYDIS_OPERAND_TYPE_REGISTER &&
			                ZydisRegisterGetClass(n.ops[0].reg.value) == ZYDIS_REGCLASS_XMM && n.ops[0].reg.value != vec;
			for (int i = 1; i < n.instr.operand_count && harmless; ++i)
				if ((n.ops[i].actions & ZYDIS_OPERAND_ACTION_MASK_WRITE) && !(n.ops[i].type == ZYDIS_OPERAND_TYPE_REGISTER &&
				                                                              n.ops[i].reg.value == ZYDIS_REGISTER_MXCSR))
					harmless = false;
			if (!harmless)
			{
				p.why = "lanes not ending in pmovsxwd: " + Text(n);
				return p;
			}
			p.seq.push_back(n);
			at += n.instr.length;
		}
		if (p.out == ZYDIS_REGISTER_NONE)
		{
			p.why = "no pmovsxwd: " + Text(d);
			return p;
		}
		for (const auto& [lane, ref] : lanes)
		{
			auto it = std::find(p.nodes.begin(), p.nodes.end(), ref.first);
			if (it == p.nodes.end())
			{
				p.nodes.push_back(ref.first);
				it = p.nodes.end() - 1;
			}
			p.lanes.push_back({static_cast<int>(it - p.nodes.begin()), ref.second});
			if (lane != static_cast<int>(p.lanes.size()) - 1)
			{
				p.why = "lanes not 0..n: " + Text(d);
				return p;
			}
		}
		if (p.nodes.size() > 2)
		{
			p.why = "more than two nodes in one vector: " + Text(d);
			return p;
		}
		p.kind = p.seq.size() == 2 && p.nodes.size() == 1 && mn == ZYDIS_MNEMONIC_MOVD ? Kind::Vector : Kind::Lanes;
		return p;
	}

	// area32(dst) = the area id of node `ref` (or of the override's pointer chain); `pushed` bytes are on the stack.
	bool EmitArea(Emitter& e, ZydisRegister dst, const Plan& p, const NodeRef* ref, int pushed)
	{
		if (p.o.chainReg != ZYDIS_REGISTER_NONE)
		{
			ZydisRegister at = p.o.chainReg;
			for (size_t i = 0; i + 1 < p.o.chain.size(); ++i)
			{
				const int64_t off = p.o.chain[i] + (at == ZYDIS_REGISTER_RSP ? pushed : 0);
				if (!e.RegMem(ZYDIS_MNEMONIC_MOV, dst, at, ZYDIS_REGISTER_NONE, 0, off, 8))
					return false;
				at = dst;
			}
			const int64_t last = p.o.chain.back() + (at == ZYDIS_REGISTER_RSP ? pushed : 0);
			return e.RegMem(ZYDIS_MNEMONIC_MOVZX, To32(dst), at, ZYDIS_REGISTER_NONE, 0, last, 2);
		}
		return e.RegMem(ZYDIS_MNEMONIC_MOVZX, To32(dst), ref->base, ref->index, ref->scale, ref->areaDisp, 2);
	}

	bool EmitTemplate(Emitter& e, const Plan& p, std::string& note)
	{
		std::set<ZydisRegister> used;
		for (const auto& d : p.seq)
			Uses(d, used);
		if (p.o.chainReg != ZYDIS_REGISTER_NONE)
			used.insert(p.o.chainReg);
		const size_t need = p.kind == Kind::Lanes ? 4 : 2;
		std::vector<ZydisRegister> s;
		for (const ZydisRegister c : kScratch)
			if (!used.count(c) && s.size() < need)
				s.push_back(c);
		if (s.size() < need)
		{
			e.error = "no free scratch registers";
			return false;
		}
		bool ok = true;
		for (const ZydisRegister r : s)
			ok = ok && e.Op(ZYDIS_MNEMONIC_PUSH, r);
		const int pushed = static_cast<int>(8 * s.size());
		switch (p.kind)
		{
		case Kind::Scalar:
		{
			ok = ok && EmitArea(e, s[0], p, &p.nodes[0], pushed);
			const Decoded& d = p.seq[0];
			const ZydisRegister dst = d.ops[0].reg.value;
			if (d.instr.mnemonic == ZYDIS_MNEMONIC_MOVZX)
			{
				// loaded sign-extended instead: the origin makes it a 32-bit absolute value
				const auto* m = MemOf(d);
				ok = ok && e.RegMem(ZYDIS_MNEMONIC_MOVSX, To32(dst), m->mem.base, m->mem.index, m->mem.scale, m->mem.disp.value, 2);
			}
			else
				ok = ok && e.Relocated(d);
			ok = ok && e.Origin(s[1], s[0], p.y);
			if (ZydisRegisterGetWidth(ZYDIS_MACHINE_MODE_LONG_64, dst) == 64)
				ok = ok && e.RegReg(ZYDIS_MNEMONIC_MOVSXD, s[1], To32(s[1]));
			ok = ok && e.RegMem(ZYDIS_MNEMONIC_LEA, dst, Widest(dst), s[1], 1, 0, 8);
			note = p.o.chainReg != ZYDIS_REGISTER_NONE ? "scalar(area chain)" : d.instr.mnemonic == ZYDIS_MNEMONIC_MOVZX ? "scalar(movzx)" : "scalar";
			break;
		}
		case Kind::Vector:
		case Kind::Pmov:
		{
			ok = ok && EmitArea(e, s[0], p, p.nodes.empty() ? nullptr : &p.nodes[0], pushed);
			for (const auto& d : p.seq)
				ok = ok && e.Relocated(d);
			ok = ok && e.LeaTable(s[1], kTable16) && e.RegMem(ZYDIS_MNEMONIC_LEA, s[0], ZYDIS_REGISTER_NONE, s[0], 8, 0, 8) &&
			     e.RegMem(ZYDIS_MNEMONIC_PADDD, p.out, s[1], s[0], 2, 0, 16);
			note = p.kind == Kind::Pmov ? "pmov(area chain)" : "vector";
			break;
		}
		case Kind::Lanes:
		{
			// the areas first (the sequence may overwrite a base register), then the sequence, then each lane
			for (size_t j = 0; j < p.nodes.size(); ++j)
				ok = ok && EmitArea(e, s[j], p, &p.nodes[j], pushed);
			for (const auto& d : p.seq)
				ok = ok && e.Relocated(d);
			for (size_t i = 0; i < p.lanes.size(); ++i)
			{
				const Lane& l = p.lanes[i];
				auto x = Emitter::Req(ZYDIS_MNEMONIC_PEXTRD);
				Emitter::Reg(x, To32(s[3]));
				Emitter::Reg(x, p.out);
				Emitter::Imm(x, i);
				ok = ok && e.Encode(x) && e.Origin(s[2], s[l.node], l.y) &&
				     e.RegMem(ZYDIS_MNEMONIC_LEA, To32(s[3]), s[3], s[2], 1, 0, 8);
				auto in = Emitter::Req(ZYDIS_MNEMONIC_PINSRD);
				Emitter::Reg(in, p.out);
				Emitter::Reg(in, To32(s[3]));
				Emitter::Imm(in, i);
				ok = ok && e.Encode(in);
			}
			note = "lanes(" + std::to_string(p.lanes.size()) + ")";
			break;
		}
		case Kind::Cmp16:
		{
			const Decoded& d = p.seq[0];
			const auto* m = MemOf(d);
			ok = ok && EmitArea(e, s[0], p, &p.nodes[0], pushed) && e.Origin(s[1], s[0], true) &&
			     e.RegMem(ZYDIS_MNEMONIC_MOVSX, To32(s[0]), m->mem.base, m->mem.index, m->mem.scale, m->mem.disp.value, 2) &&
			     e.RegMem(ZYDIS_MNEMONIC_LEA, To32(s[0]), s[0], s[1], 1, 0, 8);
			ZydisRegister q = To32(d.ops[1].reg.value);
			if (p.o.q16)
			{
				ok = ok && e.RegReg(ZYDIS_MNEMONIC_MOVSX, To32(s[1]), d.ops[1].reg.value);
				q = To32(s[1]);
			}
			ok = ok && e.RegReg(ZYDIS_MNEMONIC_CMP, To32(s[0]), q);
			note = p.o.q16 ? "cmp16(q16)" : "cmp16";
			break;
		}
		default:
			e.error = "bad plan";
			return false;
		}
		for (auto it = s.rbegin(); it != s.rend(); ++it)
			ok = ok && e.Op(ZYDIS_MNEMONIC_POP, *it);
		return ok;
	}

	struct Result
	{
		bool ok = false;
		std::string kind, note;
	};

	// One patch: the site's bytes and the ones after it until a jmp rel32 fits; every site among them gets its
	// template, the other instructions are moved over as they are.
	Result Build(const Site& s, const std::map<uint32_t, const Site*>& siteAt, const std::map<uint32_t, Overrides>& overrides, Emitter& e,
	    Patch& p, std::set<uint32_t>& handled)
	{
		auto overridesOf = [&](uint32_t rva) {
			const auto it = overrides.find(rva);
			return it == overrides.end() ? Overrides{} : it->second;
		};
		struct Item
		{
			Decoded d;     // a plain instruction
			Plan plan;     // or a site's template
			bool site = false;
		};
		std::vector<Item> items;
		uint32_t length = 0;
		while (length < 5)
		{
			Item it;
			const uint32_t at = s.rva + length;
			const auto sa = siteAt.find(at);
			const Overrides o = overridesOf(at);
			if (sa != siteAt.end() && !o.skip && o.rewrite.empty())
			{
				it.site = true;
				it.plan = Classify(*sa->second, o, at);
				if (it.plan.kind == Kind::Bad)
					return {false, it.plan.why.rfind("special", 0) == 0 ? "special" : "unpatchable", it.plan.why};
				for (const auto& d : it.plan.seq)
					length += d.instr.length;
			}
			else
			{
				if (!Decode(at, it.d))
					return {false, "?", "undecodable"};
				if (length > 0 && (it.d.instr.meta.category == ZYDIS_CATEGORY_RET || it.d.instr.mnemonic == ZYDIS_MNEMONIC_INT3))
					return {false, "?", "the function ends inside the patch"};
				length += it.d.instr.length;
			}
			items.push_back(it);
		}
		const int fi = g_image->FunctionOf(s.rva);
		if (fi >= 0)
		{
			const auto& f = g_image->Functions()[fi];
			const auto targets = BranchTargets(f.begin, f.end);
			for (uint32_t t = s.rva + 1; t < s.rva + length; ++t)
				if (targets.count(t))
					return {false, "conflict", "a branch lands inside the patched bytes at +" + std::to_string(t - s.rva)};
		}
		p.rva = s.rva;
		p.length = length;
		const uint8_t* orig = g_image->At(s.rva, length);
		p.original.assign(orig, orig + length);
		p.stub = e.Here();
		const size_t mark = e.bytes.size(), relocMark = e.relocs.size();
		std::string notes;
		bool ok = true;
		for (const Item& it : items)
		{
			if (it.site)
			{
				std::string note;
				ok = ok && EmitTemplate(e, it.plan, note);
				notes += (notes.empty() ? "" : " + ") + note;
				for (const auto& d : it.plan.seq)
					handled.insert(d.rva);
			}
			else
				ok = ok && e.Relocated(it.d);
		}
		ok = ok && e.JumpBack(s.rva + length);
		if (!ok)
		{
			e.bytes.resize(mark);
			e.relocs.resize(relocMark);
			return {false, "?", e.error};
		}
		return {true, items[0].site ? notes.substr(0, notes.find(' ')) : "?", "[" + std::to_string(length) + " B: " + notes + "]"};
	}
}

int main(int argc, char** argv)
{
	if (argc < 4)
	{
		std::fprintf(stderr, "usage: pathpatch <dump.exe> <sites.tsv> <out.inc> [overrides.tsv]\n");
		return 1;
	}
	relocate::Image image;
	std::string error;
	if (!image.Load(argv[1], error))
	{
		std::fprintf(stderr, "%s\n", error.c_str());
		return 1;
	}
	g_image = &image;
	ZydisDecoderInit(&g_decoder, ZYDIS_MACHINE_MODE_LONG_64, ZYDIS_STACK_WIDTH_64);

	std::vector<Site> sites;
	{
		std::ifstream in(argv[2]);
		for (std::string line; std::getline(in, line);)
		{
			if (line.empty() || line[0] == '#')
				continue;
			std::istringstream ss(line);
			Site s;
			std::string rva, hits;
			ss >> rva >> s.record >> s.field >> s.source >> hits >> s.cls;
			s.rva = static_cast<uint32_t>(std::stoul(rva, nullptr, 16));
			sites.push_back(s);
		}
	}
	std::map<uint32_t, Overrides> overrides;
	if (argc > 4)
	{
		std::ifstream in(argv[4]);
		for (std::string line; std::getline(in, line);)
		{
			const size_t hash = line.find('#');
			if (hash != std::string::npos)
				line.resize(hash);
			std::istringstream ss(line);
			std::string rva;
			if (!(ss >> rva))
				continue;
			std::string rest;
			std::getline(ss, rest);
			overrides[static_cast<uint32_t>(std::stoul(rva, nullptr, 16))] = ParseOverrides(rest);
		}
	}
	// an override may name a site the traces never saw (a later decode of a pair, a pointer read): add it
	for (const auto& [rva, o] : overrides)
		if (std::none_of(sites.begin(), sites.end(), [&](const Site& s) { return s.rva == rva; }))
			sites.push_back({rva, "node", "x", "override", "override"});
	std::sort(sites.begin(), sites.end(), [](const Site& a, const Site& b) { return a.rva < b.rva; });
	std::map<uint32_t, const Site*> siteAt;
	for (const Site& s : sites)
		siteAt[s.rva] = &s;

	Emitter e;
	std::vector<Patch> patches;
	std::vector<Rewrite> rewrites;
	std::map<std::string, int> counts;
	std::vector<std::string> problems;
	std::set<uint32_t> handled;
	uint32_t patchedUpTo = 0;
	for (const Site& s : sites)
	{
		char rva[16];
		std::snprintf(rva, sizeof(rva), "%#x", s.rva);
		const auto ov = overrides.find(s.rva);
		if (ov != overrides.end() && ov->second.skip)
		{
			++counts["skipped"];
			continue;
		}
		if (handled.count(s.rva))
			continue;
		if (s.rva < patchedUpTo)
		{
			++counts["overlap"];
			problems.push_back(std::string(rva) + "\t" + s.source + "\toverlap\tinside the bytes of the patch before it");
			continue;
		}
		if (ov != overrides.end() && !ov->second.rewrite.empty())
		{
			Decoded d;
			Decode(s.rva, d);
			if (ov->second.rewrite.size() > d.instr.length)
			{
				problems.push_back(std::string(rva) + "\t" + s.source + "\trewrite\tlonger than the instruction");
				continue;
			}
			Rewrite w;
			w.rva = s.rva;
			const uint8_t* orig = image.At(s.rva, d.instr.length);
			w.original.assign(orig, orig + d.instr.length);
			w.replacement = ov->second.rewrite;
			w.replacement.resize(d.instr.length, 0x90);
			rewrites.push_back(w);
			patchedUpTo = s.rva + d.instr.length;
			++counts["rewrite"];
			std::printf("%s\t%s\trewrite %s\n", rva, s.source.c_str(), Text(d).c_str());
			continue;
		}
		Patch p;
		const Result r = Build(s, siteAt, overrides, e, p, handled);
		if (!r.ok)
		{
			++counts[r.kind];
			problems.push_back(std::string(rva) + "\t" + s.source + "\t" + r.kind + "\t" + r.note);
			continue;
		}
		if (p.length > 48)
		{
			++counts["too long"];
			problems.push_back(std::string(rva) + "	" + s.source + "	too long	more than 48 bytes");
			continue;
		}
		patchedUpTo = p.rva + p.length;
		++counts[r.kind];
		Decoded d;
		Decode(s.rva, d);
		std::printf("%s\t%s\t%s  %s\n", rva, s.source.c_str(), Text(d).c_str(), r.note.c_str());
		patches.push_back(std::move(p));
	}
	std::printf("\n%zu sites handled; %zu patches, %zu rewrites, %zu stub bytes, %zu relocations\n", handled.size(), patches.size(), rewrites.size(),
	    e.bytes.size(), e.relocs.size());
	for (const auto& [k, n] : counts)
		std::printf("  %-22s %d\n", k.c_str(), n);
	std::printf("\nnot patched:\n");
	for (const auto& pr : problems)
		std::printf("  %s\n", pr.c_str());

	std::ofstream out(argv[3], std::ios::binary);
	out << "// Generated by tools/pathpatch from the game build's dump, research/pathsites.tsv and research/pathsites_overrides.tsv:\n";
	out << "// do not edit. " << patches.size() << " stubs and " << rewrites.size()
	    << " rewrites for the road network's node/junction position reads (research/phase0.md §61).\n";
	out << "constexpr uint32_t kPathTable16 = " << kTable16 << ", kPathTable8 = " << kTable8 << ", kPathStubs = " << kStubs << ";\n";
	out << "constexpr uint8_t kPathStubBytes[] = {";
	for (size_t i = 0; i < e.bytes.size(); ++i)
		out << (i % 32 ? "" : "\n\t") << static_cast<int>(e.bytes[i]) << ",";
	out << "\n};\n";
	out << "struct PathReloc\n{\n\tuint32_t at, target, end;\n};\nconstexpr PathReloc kPathRelocs[] = {";
	for (size_t i = 0; i < e.relocs.size(); ++i)
		out << (i % 4 ? " " : "\n\t") << "{" << e.relocs[i].at << ", " << e.relocs[i].target << ", " << e.relocs[i].end << "},";
	out << "\n};\n";
	out << "struct PathSite\n{\n\tuint32_t rva, length, stub;\n\tuint8_t original[48];\n};\nconstexpr PathSite kPathSites[] = {";
	for (const Patch& p : patches)
	{
		out << "\n\t{" << p.rva << ", " << p.length << ", " << p.stub << ", {";
		for (size_t i = 0; i < p.original.size() && i < 48; ++i)
			out << static_cast<int>(p.original[i]) << ",";
		out << "}},";
	}
	out << "\n};\n";
	out << "struct PathRewrite\n{\n\tuint32_t rva, length;\n\tuint8_t original[16], replacement[16];\n};\nconstexpr PathRewrite kPathRewrites[] = {";
	for (const Rewrite& w : rewrites)
	{
		out << "\n\t{" << w.rva << ", " << w.original.size() << ", {";
		for (const uint8_t b : w.original)
			out << static_cast<int>(b) << ",";
		out << "}, {";
		for (const uint8_t b : w.replacement)
			out << static_cast<int>(b) << ",";
		out << "}},";
	}
	out << "\n};\n";

	// The region grid (1024 m regions instead of 512 m, from -16384): every rip-relative read of the grid's constants in
	// the path code is pointed at our own copy of it, scaled. They go in after the stubs, 16-byte aligned. A float the
	// code holds as an immediate ("imm") is rewritten in place.
	struct Redirect
	{
		uint32_t rva, length, dispOffset, constant;
		std::vector<uint8_t> original;
	};
	struct Immediate
	{
		uint32_t rva, length, immOffset, value;
		std::vector<uint8_t> original;
	};
	std::vector<Redirect> redirects;
	std::vector<Immediate> immediates;
	std::vector<std::array<float, 4>> constants;
	// one lane scaled: 8192, -8192, 512 and 256 doubled, 1/512 halved, 0 kept; anything else is not the grid's
	const auto scale = [](float v, float& out) {
		if (v == 8192.0f || v == -8192.0f || v == 512.0f || v == 256.0f)
			out = v * 2;
		else if (v == 1.0f / 512)
			out = 1.0f / 1024;
		else if (v == 0.0f)
			out = 0.0f;
		else
			return false;
		return true;
	};
	if (argc > 5)
	{
		std::ifstream in(argv[5]);
		for (std::string line; std::getline(in, line);)
		{
			if (line.empty() || line[0] == '#')
				continue;
			std::istringstream fields(line);
			std::string at, kind;
			fields >> at >> kind;
			const uint32_t rva = static_cast<uint32_t>(std::stoul(at, nullptr, 16));
			Decoded d;
			if (kind == "imm")
			{
				int op = -1;
				if (Decode(rva, d))
					for (int i = 0; i < d.instr.operand_count; ++i)
						if (d.ops[i].type == ZYDIS_OPERAND_TYPE_IMMEDIATE && d.ops[i].size == 32)
							op = i;
				float v = 0, scaled = 0;
				const uint32_t bits = op >= 0 ? static_cast<uint32_t>(d.ops[op].imm.value.u) : 0;
				std::memcpy(&v, &bits, 4);
				if (op < 0 || d.instr.raw.imm[0].size != 32 || !scale(v, scaled) || v == 0.0f)
				{
					std::printf("grid %#x: not an immediate of the region grid\n", rva);
					continue;
				}
				uint32_t value = 0;
				std::memcpy(&value, &scaled, 4);
				const uint8_t* orig = image.At(rva, d.instr.length);
				immediates.push_back({rva, d.instr.length, d.instr.raw.imm[0].offset, value, std::vector<uint8_t>(orig, orig + d.instr.length)});
				continue;
			}
			if (!Decode(rva, d) || !(d.instr.attributes & ZYDIS_ATTRIB_IS_RELATIVE) || d.instr.raw.disp.size != 32)
			{
				std::printf("grid %#x: not a rip-relative read\n", rva);
				continue;
			}
			uint64_t target = 0;
			int lanes = 1; // the floats the instruction reads
			for (int i = 0; i < d.instr.operand_count; ++i)
				if (d.ops[i].type == ZYDIS_OPERAND_TYPE_MEMORY && d.ops[i].mem.base == ZYDIS_REGISTER_RIP)
				{
					ZydisCalcAbsoluteAddress(&d.instr, &d.ops[i], rva, &target);
					lanes = std::clamp(d.ops[i].size / 32, 1, 4);
				}
			const float* v = reinterpret_cast<const float*>(image.At(static_cast<uint32_t>(target), 16));
			std::array<float, 4> c{};
			bool grid = true;
			for (int l = 0; l < lanes && grid; ++l)
				grid = scale(v[l], c[l]);
			if (!grid || std::all_of(c.begin(), c.begin() + lanes, [](float x) { return x == 0.0f; }))
			{
				std::printf("grid %#x: constant %g is not the region grid's\n", rva, v[0]);
				continue;
			}
			auto it = std::find(constants.begin(), constants.end(), c);
			if (it == constants.end())
			{
				constants.push_back(c);
				it = constants.end() - 1;
			}
			const uint8_t* orig = image.At(rva, d.instr.length);
			redirects.push_back({rva, d.instr.length, d.instr.raw.disp.offset, static_cast<uint32_t>(it - constants.begin()),
			    std::vector<uint8_t>(orig, orig + d.instr.length)});
		}
		std::printf("\n%zu grid constant reads redirected to %zu constants, %zu immediates rewritten\n", redirects.size(), constants.size(),
		    immediates.size());
	}
	auto literal = [](float v) {
		char buf[32];
		std::snprintf(buf, sizeof(buf), "%.10g", v);
		std::string t = buf;
		if (t.find_first_of(".e") == std::string::npos)
			t += ".0";
		return t + "f";
	};
	out << "constexpr float kPathGridConstants[][4] = {";
	for (const auto& c : constants)
		out << "\n\t{" << literal(c[0]) << ", " << literal(c[1]) << ", " << literal(c[2]) << ", " << literal(c[3]) << "},";
	if (constants.empty())
		out << "{}";
	out << "\n};\n";
	out << "struct PathRedirect\n{\n\tuint32_t rva, length, dispOffset, constant;\n\tuint8_t original[16];\n};\nconstexpr PathRedirect kPathGrid[] = {";
	for (const Redirect& r : redirects)
	{
		out << "\n\t{" << r.rva << ", " << r.length << ", " << r.dispOffset << ", " << r.constant << ", {";
		for (const uint8_t b : r.original)
			out << static_cast<int>(b) << ",";
		out << "}},";
	}
	out << "\n};\n";
	out << "struct PathGridImmediate\n{\n\tuint32_t rva, length, immOffset, value;\n\tuint8_t original[16];\n};\n"
	       "constexpr PathGridImmediate kPathGridImmediates[] = {";
	for (const Immediate& m : immediates)
	{
		out << "\n\t{" << m.rva << ", " << m.length << ", " << m.immOffset << ", " << m.value << "u, {";
		for (const uint8_t b : m.original)
			out << static_cast<int>(b) << ",";
		out << "}},";
	}
	if (immediates.empty())
		out << "{}";
	out << "\n};\n";
	return 0;
}
