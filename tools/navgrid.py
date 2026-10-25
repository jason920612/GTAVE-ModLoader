"""The navmesh grid's growth (research/phase0.md §61.8): the code sites to change for W x H squares of 150 m from
(-6000, -6000) instead of 100 x 100, found in the unpacked dump. Writes research/navgrid.tsv (what and why),
research/navscale.tsv (the boxes' scale) and src/loader/game/navgrid_data.inc (the loader checks each instruction's
bytes, then rewrites its operand).

Classes: dyn-base (the first dynamic navmesh, 10000 -> W*H), dyn-check (x - 10000 before a range check), grid-last
(9999 -> W*H - 1), special (10100 -> W*H + 100; special<<16: in a polygon's +4 dword, its navmesh index in the high
word), store-size / store-count (the navmesh stores' name table: 10000 entries -> 16384), grid-size (nav.dat's reader:
W, H), max-index (MAX_NUM_NAVMESHES_IN_ANY_LEVEL -> the last index), row-length (an index taken apart with H where W is
meant). Scale sites: quant (x 4 -> x 2), dequant (x 0.25 -> x 0.5, x 0.125 -> x 0.25), clamp (+-8191 -> +-16383).
usage: python -I tools/navgrid.py <dump.exe> [W H]"""
import bisect, collections, struct, sys

import capstone

DUMP = sys.argv[1]
W, H = (int(sys.argv[2]), int(sys.argv[3])) if len(sys.argv) > 3 else (147, 110)
GRID = W * H
DYN, SPECIAL = GRID, GRID + 100
assert SPECIAL < 0x3FFF, "navmesh indices are 14-bit"
CAPACITY = 16384
assert GRID <= CAPACITY
STORE_SIZE, COUNT_AT = 0x104 + 8 * CAPACITY + 8, 0x104 + 8 * CAPACITY

d = open(DUMP, "rb").read()
pe = struct.unpack_from("<I", d, 0x3C)[0]
exc_rva, exc_size = struct.unpack_from("<II", d, pe + 24 + 112 + 3 * 8)
funcs = sorted(struct.unpack_from("<III", d, exc_rva + i * 12) for i in range(exc_size // 12))
md = capstone.Cs(capstone.CS_ARCH_X86, capstone.CS_MODE_64)
md.detail = True


def navmesh_code(a):
    # the navmesh store and streaming (0xCC0000..) and the navmesh / path server code (0x1100000..)
    return 0xCC0000 <= a < 0xD00000 or 0x1100000 <= a < 0x1180000


# 10000 that is not a navmesh index: a debug object's member, timers (x - 10000 ms, 10000 / 5000 ms choices)
NOT_NAV = set(range(0x1120EA9, 0x11212ED)) | {0x1112FC5, 0x11242F3, 0xD67381, 0xD6744B, 0xD675DF, 0xEB92A0, 0xB31AE1}
EXPLICIT = {0xCDDC33: "dyn-base", 0xCDDCEC: "dyn-base", 0xCDDEE1: "dyn-base", 0xCDDF0C: "dyn-base", 0x112BD9D: "dyn-base",
            0x1132715: "dyn-base", 0xCDDF11: "dyn-base<<16", 0xCDD96D: "grid-last", 0xCDDB5D: "grid-last"}
# 10100 outside the navmesh code: comparisons of navmesh indices (reviewed one by one)
SPECIAL_OUTSIDE = {0x13C13D1, 0x198BB1F, 0x198CCFB, 0x1CFC78D, 0x1D92387, 0x1D928A9, 0x1D92ADE, 0x1D92D05, 0x1D9351E, 0x1D93CEB,
                   0x1D9433A, 0x1D947BD, 0x1D9491A}
NEW = {"dyn-base": DYN, "dyn-check": -DYN, "grid-last": GRID - 1, "special": SPECIAL, "dyn-base<<16": DYN << 16, "special<<16": SPECIAL << 16}


def operand_offset(raw, value, size):
    enc = struct.pack("<i" if value < 0 else "<I", value) if size == 4 else struct.pack("<q" if value < 0 else "<Q", value)
    k = raw.find(enc)
    return k if k >= 0 and raw.find(enc, k + 1) < 0 else None


rows = []
for b, e, u in funcs:
    if e - b > 0x100000:
        continue
    ins_list = list(md.disasm(d[b:e], b))
    for i, ins in enumerate(ins_list):
        a = ins.address
        if a in NOT_NAV:
            continue
        for op in ins.operands:
            if op.type == capstone.x86.X86_OP_IMM:
                val = op.imm
                if op.size == 4 and val >= 0x80000000:
                    val -= 1 << 32
            elif op.type == capstone.x86.X86_OP_MEM and op.mem.base != capstone.x86.X86_REG_RIP:
                val = op.mem.disp
            else:
                continue
            cls = None
            if a in EXPLICIT and val in (0x2710, 0x270F, 0x27100000):
                cls = EXPLICIT[a]
            elif val == 0x2774 and op.type == capstone.x86.X86_OP_IMM and (navmesh_code(a) or a in SPECIAL_OUTSIDE):
                cls = "special"
            elif val == 0x27740000 and op.type == capstone.x86.X86_OP_IMM and navmesh_code(a):
                cls = "special<<16"  # a polygon's +4 dword: its first index, its navmesh index in the high word
            elif (val == -0x2710 and ins.mnemonic in ("lea", "add")) or (val == 0x2710 and ins.mnemonic == "sub" and op.type == capstone.x86.X86_OP_IMM):
                checked = any(n.mnemonic == "cmp" and n.op_str.endswith(", 0x63") for n in ins_list[i + 1:i + 4])
                if navmesh_code(a) or checked:
                    cls = "dyn-check"
            if not cls:
                continue
            raw = bytes(ins.bytes)
            new = NEW[cls] if cls != "dyn-check" else (-DYN if val < 0 else DYN)
            off = operand_offset(raw, val, 4)
            if off is None:
                sys.exit(f"cannot place the operand of {a:#x} {ins.mnemonic} {ins.op_str}")
            rows.append((a, off, 4, val, new, cls, raw, f"{ins.mnemonic} {ins.op_str}"))


def site(a, value, size, new, cls):
    ins = next(md.disasm(d[a:a + 16], a))
    raw = bytes(ins.bytes)
    off = operand_offset(raw, value, size)
    assert off is not None, hex(a)
    rows.append((a, off, size, value, new, cls, raw, f"{ins.mnemonic} {ins.op_str}"))


# the navmesh stores (NavMeshes, HeightMeshes): 0x13988 bytes, a name table (name hash -> index, a square each) of
# 10000 entries at +0x104 and its count at +0x13984
for a in (0x11323B1, 0x1132478):
    site(a, 0x13988, 4, STORE_SIZE, "store-size")
for a in (0xCDD6B9, 0xCDD92B, 0xCDDA1D, 0xCDDA8B, 0xCDDA95, 0xCDDAC3, 0xCDDB1B):
    site(a, 0x13984, 4, COUNT_AT, "store-count")
# a navmesh index back to its square: the game divides and multiplies by the store's H (+0xC0) where the row length is W
# (+0xBC): the same while the grid was square
for a in (0xCF8F7B, 0xCF93BA):
    site(a, 0xC0, 4, 0xBC, "row-length")
# nav.dat's reader: the squares (W and H as two dwords); MAX_NUM_NAVMESHES_IN_ANY_LEVEL's value replaced by the last index
site(0x112BD5A, 0x6400000064, 8, (H << 32) | W, "grid-size")
max_raw = d[0x112BE30:0x112BE37]
assert max_raw == bytes.fromhex("8b442434894714"), max_raw.hex()  # mov eax,[rsp+34h]; mov [rdi+14h],eax
rows.append((0x112BE30, 0, 7, 0, SPECIAL - 1, "max-index", max_raw, f"mov dword [rdi+14h], {SPECIAL - 1} (was the file's value)"))

# The boxes in quarter metres: polygons' (+0x18, x0 x1 y0 y1 z0 z1) and quadtree nodes' (+0x20) int16 boxes, and the path
# server's own (dynamic objects, queries), are positions x 4: +-8192 m. With the wider grid they are positions x 2
# (+-16384 m; the placement hook re-encodes the files' boxes). Changed: every constant with a 4.0 lane that scales a value
# converted to an integer in the navmesh code (found backwards from each float->int conversion, along every path and
# register copy), the reviewed int->float conversions of those boxes (x 0.25; x 0.125 for the mean of two) and the
# clamps to +-8191 m before a conversion. Each such instruction reads a scaled copy of its constant instead.
SCALE_CODE = [(0xCC0000, 0xD10000), (0x1100000, 0x1180000), (0x1D80000, 0x1DA0000)]
SCALE_FUNCS = {0x13C1090}  # a polygon box query outside those ranges
NOT_BOX = {0x1121C9A}  # an entity's +0xA8 (its own quarter metres, not a navmesh box)
DEQUANT = {0xCD8D53, 0xCD8D89, 0x1134400, 0x113A60B, 0x113EE26, 0x113EE2E, 0x113F870, 0x114C2E1, 0x1151215, 0x115E793,
           0x115F32F, 0x115F386, 0x115F3D1, 0x1D8FB01}
CLAMP = {0xCD9E59, 0xCD9E6C, 0xCD9E8A, 0xCD9EC5, 0xCD9ED7, 0x1138A2D, 0x1138A35, 0x11398E8, 0x113991B, 0x11483BF, 0x11486FF,
         0x13C1404, 0x13C142E}
SCALE = {4.0: 2.0, 0.25: 0.5, 0.125: 0.25, 8191.0: 16383.0, -8191.0: -16383.0}
COPIES = ("movaps", "movups", "movdqa", "movdqu", "movapd")
VOLATILE = {f"xmm{i}" for i in range(6)}


def primary(b, u):
    while (d[u] >> 3) & 4:  # chained unwind info: the function's first part
        cnt = d[u + 2]
        b, _, u = struct.unpack_from("<III", d, u + 4 + ((cnt + 1) & ~1) * 2)
    return b


parts = collections.defaultdict(list)
for b, e, u in funcs:
    parts[primary(b, u)].append((b, e))


def reg_name(op):
    return md.reg_name(op.reg)


def rip_constant(x):
    for o in x.operands:
        if o.type == capstone.x86.X86_OP_MEM and o.mem.base == capstone.x86.X86_REG_RIP and o.size in (4, 16):
            t = x.address + x.size + o.mem.disp
            return t, struct.unpack_from("<4f" if o.size == 16 else "<f", d, t)
    return None


def definitions(ins, preds, at, reg):
    """the instructions defining reg that reach `at`, through register copies (address, register; "call": clobbered)"""
    out, seen, work = [], set(), [(p, reg) for p in preds[at]]
    while work:
        a, r = work.pop()
        if (a, r) in seen:
            continue
        seen.add((a, r))
        x = ins[a]
        ops = x.operands
        if ops and ops[0].type == capstone.x86.X86_OP_REG and reg_name(ops[0]) == r and not x.mnemonic.startswith(("ucomi", "comi", "cmp", "test")):
            if x.mnemonic in COPIES and len(ops) == 2 and ops[1].type == capstone.x86.X86_OP_REG:
                work.extend((p, reg_name(ops[1])) for p in preds[a])
            else:
                out.append((a, r))
            continue
        if x.mnemonic == "call" and r in VOLATILE:
            out.append((a, "call"))
            continue
        work.extend((p, r) for p in preds[a])
    return out


quant = set()
for f, fparts in parts.items():
    if not (any(lo <= f < hi for lo, hi in SCALE_CODE) or f in SCALE_FUNCS):
        continue
    ins = {}
    for b, e in fparts:
        for x in md.disasm(d[b:e], b):
            ins[x.address] = x
    preds = collections.defaultdict(list)
    for a, x in ins.items():
        targets = []
        if x.mnemonic.startswith("j") and x.operands and x.operands[0].type == capstone.x86.X86_OP_IMM:
            targets.append(x.operands[0].imm)
        if x.mnemonic not in ("jmp", "ret", "int3", "ud2"):
            targets.append(a + x.size)
        for t in targets:
            if t in ins:
                preds[t].append(a)
    for a, x in ins.items():
        if x.mnemonic not in ("cvttps2dq", "cvttss2si") or len(x.operands) < 2 or x.operands[1].type != capstone.x86.X86_OP_REG:
            continue
        for m, _ in definitions(ins, preds, a, reg_name(x.operands[1])):
            if m == "call" or not isinstance(m, int) or ins[m].mnemonic not in ("mulps", "mulss"):
                continue
            y = ins[m]
            if rip_constant(y):
                candidates = [m]
            elif len(y.operands) == 2 and y.operands[1].type == capstone.x86.X86_OP_REG:
                candidates = [c for c, r in definitions(ins, preds, m, reg_name(y.operands[1])) if r != "call"]
            else:
                candidates = []
            for c in candidates:
                k = rip_constant(ins[c])
                if k and 4.0 in k[1] and c not in NOT_BOX:
                    quant.add(c)

scale_rows = []
for a in sorted(quant | DEQUANT | CLAMP):
    x = next(md.disasm(d[a:a + 16], a))
    t, values = rip_constant(x)
    kind = "quant" if a in quant else "dequant" if a in DEQUANT else "clamp"
    want = {"quant": (4.0,), "dequant": (0.25, 0.125), "clamp": (8191.0, -8191.0)}[kind]
    assert any(v in want for v in values), f"{a:#x}: {values}"
    raw = bytes(x.bytes)
    disp = struct.pack("<i", t - (a + x.size))
    k = raw.find(disp)
    assert k > 0 and raw.find(disp, k + 1) < 0, hex(a)
    lanes = struct.unpack_from("<4f", d, t) if len(values) == 4 else (values[0], 0.0, 0.0, 0.0)  # (a scalar reads lane 0)
    new = tuple(SCALE.get(v, v) for v in lanes)
    scale_rows.append((a, kind, raw, k, values, new, f"{x.mnemonic} {x.op_str}"))
constants = sorted({r[5] for r in scale_rows})
print(collections.Counter(r[1] for r in scale_rows), len(scale_rows), "scale sites,", len(constants), "constants")
with open("research/navscale.tsv", "w", encoding="utf-8", newline="\n") as out:
    out.write("# The navmesh boxes' scale (research/phase0.md §61.8): quarter metres -> half metres. Generated by tools/navgrid.py.\n")
    out.write("# rva\tkind\tdisplacement offset\toriginal bytes\tconstant read\tits scaled copy\tinstruction\n")
    for a, kind, raw, k, values, new, text in scale_rows:
        out.write(f"{a:#x}\t{kind}\t{k}\t{raw.hex()}\t{', '.join(f'{v:g}' for v in values)}\t{', '.join(f'{v:g}' for v in new)}\t{text}\n")

rows.sort()
assert len({r[0] for r in rows}) == len(rows), "a site twice"
print(collections.Counter(r[5] for r in rows), len(rows))
with open("research/navgrid.tsv", "w", encoding="utf-8", newline="\n") as out:
    out.write(f"# The navmesh grid's growth (research/phase0.md §61.8): 100 x 100 -> {W} x {H} squares; dynamic navmeshes 10000.. -> {DYN}..,\n")
    out.write(f"# the special index 10100 -> {SPECIAL}. Generated by tools/navgrid.py from the dump.\n")
    out.write("# rva\toperand offset\tsize\told\tnew\tclass\toriginal bytes\tinstruction\n")
    for a, off, size, old, new, cls, raw, text in rows:
        out.write(f"{a:#x}\t{off}\t{size}\t{old}\t{new}\t{cls}\t{raw.hex()}\t{text}\n")
with open("src/loader/game/navgrid_data.inc", "w", encoding="utf-8", newline="\n") as out:
    out.write(f"// Generated by tools/navgrid.py: {len(rows)} sites for a navmesh grid of {W} x {H} (research/navgrid.tsv).\n")
    out.write(f"constexpr uint32_t kNavGridWidth = {W}, kNavGridHeight = {H}, kNavGridFirstDynamic = {DYN}, kNavGridSpecial = {SPECIAL};\n")
    out.write("struct NavGridSite\n{\n\tuint32_t rva;\n\tuint8_t length, offset, size;\n\tint64_t value;\n\tuint8_t original[16];\n"
              "\tuint8_t lead[4]; // (when set: the instruction replaced by these bytes and the value)\n};\n")
    out.write("constexpr NavGridSite kNavGridSites[] = {\n")
    for a, off, size, old, new, cls, raw, text in rows:
        lead = ""
        if cls == "max-index":
            # the 7 bytes become mov dword ptr [rdi+14h], imm32
            off, size, lead = 3, 4, "0xC7, 0x47, 0x14"
        out.write(f"\t{{{a:#x}, {len(raw)}, {off}, {size}, {new}, {{{', '.join(str(x) for x in raw)}}}, {{{lead}}}}},\n")
    out.write("};\n")
    out.write(f"// The boxes' scale (research/navscale.tsv): {len(scale_rows)} instructions read a scaled copy of their constant.\n")
    out.write("constexpr float kNavScaleConstants[][4] = {\n")
    for c in constants:
        out.write(f"\t{{{', '.join(f'{v:.1f}f' if v == int(v) else f'{v!r}f' for v in c)}}},\n")
    out.write("};\n")
    out.write("struct NavScaleSite\n{\n\tuint32_t rva;\n\tuint8_t length, dispOffset, constant;\n\tuint8_t original[16];\n};\n")
    out.write("constexpr NavScaleSite kNavScaleSites[] = {\n")
    for a, kind, raw, k, values, new, text in scale_rows:
        out.write(f"\t{{{a:#x}, {len(raw)}, {k}, {constants.index(new)}, {{{', '.join(str(x) for x in raw)}}}}}, // {kind}\n")
    out.write("};\n")
