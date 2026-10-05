"""Convert a legacy fragment (.yft, resource version 162) to GTA V Enhanced (version 171).
usage: convert_yft.py <legacy.yft> <out.yft> [--effects effects.json]

Everything down to the geometry has the same layout in both versions (research/phase0.md §15-16), so the
legacy virtual block is kept where it is and only these are rewritten:
  vertex buffer   legacy 0x80 -> Enhanced 0x40 + 0x20 view, in place; vertex data reordered in place
  index buffer    legacy 0x60 -> Enhanced 0x40 + 0x20 view, in place
  declaration     legacy 0x10 -> Enhanced 0x140, appended
  shader          legacy 0x30 -> Enhanced 0x40 + parameter table ("meta") + data, appended. The game matches the
                  table against the effect by name, so legacy constants keep their values; texture slots are
                  taken in the effect's order (legacy sampler names have no other link to Enhanced textures).
                  +0x3A is the size of the parameter block at +0x08, which must fit the effect's block.
  texture ref     0x50 both, in place
  page map        appended with room for every page
New data goes into extra pages of the block's base size after the legacy pages; a fractional last page is
promoted to a full one first, so no legacy offset changes.
"""
import json, os, struct, sys, zlib

sys.path.insert(0, os.path.dirname(__file__))
from convert_ytd import block_size, page_count

VIRTUAL = 0x50000000
LEGACY_VERSION, ENHANCED_VERSION = 162, 171

# Enhanced file-form vtables (build base 0x140000000; the game replaces them, but keep them realistic)
VT_VERTEX_BUFFER, VT_INDEX_BUFFER, VT_VIEW, VT_TEXTURE_REF = 0x1406B9108, 0x1406B90B8, 0x1406B77D8, 0x1406B7940

# legacy declaration bit -> Enhanced slot (§15)
BIT_TO_SLOT = {0: 0, 1: 16, 2: 20, 3: 4, 4: 24, 5: 25, 14: 8, 15: 12, **{6 + i: 28 + i for i in range(8)}}
# legacy element type -> (bytes, DXGI format)
LEGACY_TYPES = {0: (2, 54), 1: (4, 34), 3: (8, 10), 4: (4, 41), 5: (8, 16), 6: (12, 6), 7: (16, 2), 8: (4, 30), 9: (4, 28)}
SLOT_COUNT = 36

def u8(b, o): return b[o]
def u16(b, o): return struct.unpack_from("<H", b, o)[0]
def u32(b, o): return struct.unpack_from("<I", b, o)[0]
def u64(b, o): return struct.unpack_from("<Q", b, o)[0]

def page_list(flags):
    """Page sizes in file order (largest class first), the game's own order."""
    base = 0x2000 << (flags & 0xF)
    sizes = [base * 16] * ((flags >> 4) & 1) + [base * 8] * ((flags >> 5) & 3) + [base * 4] * ((flags >> 7) & 0xF) + \
            [base * 2] * ((flags >> 11) & 0x3F) + [base] * ((flags >> 17) & 0x7F)
    for bit, div in ((0x1000000, 2), (0x2000000, 4), (0x4000000, 8), (0x8000000, 16)):
        if flags & bit:
            sizes.append(base // div)
    return sizes

class Block:
    """The virtual block: legacy pages kept in place, new data in appended pages of the base size."""
    def __init__(self, data, flags):
        self.base = 0x2000 << (flags & 0xF)
        sizes = page_list(flags)
        fractional = [s for s in sizes if s < self.base]
        if len(fractional) > 1:
            sys.exit("more than one fractional page: not supported yet")
        self.flags = flags & ~0x0F000000 & 0x0FFFFFFF  # drop fractional bits and version nibble
        if fractional:
            self.flags += 1 << 17                      # the fractional page becomes a full one in place
        self.data = bytearray(data) + bytearray(block_size(self.flags) - len(data))
        self.free = 0                                  # bytes left in the last appended page

    def alloc(self, size, align=16):
        assert size <= self.base, size
        if self.free:
            used = self.base - self.free
            start = (used + align - 1) & ~(align - 1)
            if start + size <= self.base:
                self.free = self.base - start - size
                return len(self.data) - self.base + start
        if (self.flags >> 17) & 0x7F == 0x7F:
            sys.exit("out of page slots")
        self.flags += 1 << 17
        self.data += bytearray(self.base)
        self.free = self.base - size
        return len(self.data) - self.base

    def ptr(self, p):
        return p - VIRTUAL if p and VIRTUAL <= p < VIRTUAL + len(self.data) else None

def convert_declaration(b, o):
    mask, stride, _, count, types = struct.unpack_from("<IHBBQ", b, o)
    elements = []  # (legacy offset, size, slot, format)
    offset = 0
    for bit in range(16):
        if mask & (1 << bit):
            size, fmt = LEGACY_TYPES[(types >> (4 * bit)) & 0xF]
            slot = BIT_TO_SLOT[bit]
            if slot == 20 and size == 4:
                fmt = 30  # blend indices are read as R8G8B8A8_UINT
            elements.append((offset, size, slot, fmt))
            offset += size
    if offset != stride:
        sys.exit(f"declaration {o:#x}: elements add up to {offset:#x}, stride {stride:#x}")
    elements.sort(key=lambda e: e[2])
    decl = bytearray(0x140)
    offsets, at = [], 0
    present = {e[2]: e for e in elements}
    for slot in range(52):
        offsets.append(at)
        if slot in present:
            at += present[slot][1]
    struct.pack_into("<52I", decl, 0, *offsets)
    for slot, (_, size, _, fmt) in present.items():
        decl[0xD0 + slot] = stride
        decl[0x104 + slot] = fmt
    struct.pack_into("<I", decl, 0x138, stride << 2)
    return decl, elements, stride

def reorder_vertices(b, data, count, stride, elements):
    for v in range(count):
        o = data + v * stride
        src = bytes(b[o:o + stride])
        out = bytearray()
        for legacy_off, size, _, _ in elements:
            out += src[legacy_off:legacy_off + size]
        b[o:o + stride] = out

def find_shader_groups(blk):
    b, found = blk.data, {}
    for o in range(0, len(b) - 0x20, 16):
        arr = blk.ptr(u64(b, o + 0x10))
        n, cap = u16(b, o + 0x18), u16(b, o + 0x1A)
        if arr is None or not 0 < n <= cap <= 1024:
            continue
        shaders = [blk.ptr(u64(b, arr + 8 * i)) for i in range(n)]
        if all(s is not None and blk.ptr(u64(b, s)) is not None and b[s + 0x13] == 0x80 for s in shaders):
            found[o] = (arr, shaders)
    return found

def find_geometries(blk):
    b, geos = blk.data, []
    def is_vb(o):
        d = blk.ptr(u64(b, o + 0x10))
        dc = blk.ptr(u64(b, o + 0x30))
        return d is not None and u64(b, o + 0x20) == u64(b, o + 0x10) and dc is not None and u16(b, dc + 4) == u16(b, o + 8)
    for o in range(0, len(b) - 0x80, 16):
        vb, ib = blk.ptr(u64(b, o + 0x18)), blk.ptr(u64(b, o + 0x38))
        if vb is not None and ib is not None and is_vb(vb) and blk.ptr(u64(b, ib + 0x10)) is not None and u32(b, ib + 8):
            geos.append((o, vb, ib))
    return geos

def legacy_params(b, blk, s):
    params = blk.ptr(u64(b, s))
    count = b[s + 0x10]
    types = [b[params + 16 * i] for i in range(count)]
    hashes_at = params + 16 * count + sum(16 * t for t in types)
    out = []
    for i, t in enumerate(types):
        name = u32(b, hashes_at + 4 * i)
        p = u64(b, params + 16 * i + 8)
        out.append((name, t, blk.ptr(p) if p else None))
    return out

def convert_shader(blk, s, effects, report):
    b = blk.data
    effect = u32(b, s + 8)
    info = effects.get(f"{effect:08x}")
    if info is None:
        sys.exit(f"shader {s:#x}: effect {effect:08x} is not an Enhanced effect")
    params = legacy_params(b, blk, s)
    textures = [(n, p) for n, t, p in params if t == 0]
    constants = [(n, t, p) for n, t, p in params if t > 0]

    # Constant data: one block, legacy values back to back (n float4s each).
    cdata, entries = bytearray(), []
    for name, t, p in constants:
        size = 16 * t
        renamed = int(info["constants"].get(f"{name:08x}", f"{name:08x}"), 16)
        entries.append((renamed, 3 | (0 << 2) | (len(cdata) << 8) | (size << 20)))
        cdata += b[p:p + size] if p is not None else bytes(size)
    if len(cdata) > 0xFFF:
        sys.exit(f"shader {s:#x}: {len(cdata):#x} bytes of constants")
    tex_names = info["textures"]
    if len(textures) > len(tex_names):
        report.append(f"effect {effect:08x}: {len(textures)} legacy textures, effect has {len(tex_names)}")
    tex_entries = []
    for i, (name, p) in enumerate(textures[:len(tex_names)]):
        tex_entries.append((int(tex_names[i], 16), 0 | (i << 2)))
    meta_entries = tex_entries + entries

    # +0x08 points at the instance's parameter block: our own pointer array and data at first, which the game
    # reads through the table and then overwrites with the effect's layout. It must hold the effect's block
    # (size at +0x3A, as in the game's own files) or the game keeps a separate allocation.
    block = max(info["block"], 0x10 + len(cdata))
    block = (block + 15) & ~15
    if block > 0xFFFF:
        sys.exit(f"shader {s:#x}: parameter block of {block:#x} bytes")
    inst = blk.alloc(0x40)
    meta = blk.alloc(8 + 8 * len(meta_entries))
    cb_array = blk.alloc(block)
    cb = cb_array + 0x10
    tex_array = blk.alloc(8 * max(1, len(tex_entries)))
    b = blk.data  # alloc may have grown the block

    struct.pack_into("<BBBBBBBB", b, meta, 1, len(tex_entries), 0, 0, len(meta_entries), 0, 0, 0x0C)
    for i, (h, inf) in enumerate(meta_entries):
        struct.pack_into("<II", b, meta + 8 + 8 * i, h, inf)
    struct.pack_into("<Q", b, cb_array, VIRTUAL + cb)
    b[cb:cb + len(cdata)] = cdata
    for i, (_, p) in enumerate(textures[:len(tex_entries)]):
        struct.pack_into("<Q", b, tex_array + 8 * i, VIRTUAL + p if p is not None else 0)
        if p is not None:
            convert_texture_ref(b, p)

    struct.pack_into("<II", b, inst, effect, 0x6D657461)
    struct.pack_into("<QQQQ", b, inst + 0x08, VIRTUAL + cb_array, VIRTUAL + tex_array, 0, VIRTUAL + meta)
    struct.pack_into("<H", b, inst + 0x3A, block)
    b[inst + 0x39] = b[s + 0x11]                 # draw bucket
    b[inst + 0x3C:inst + 0x40] = b[s + 0x20:s + 0x24]
    return inst

def convert_texture_ref(b, o):
    if u32(b, o + 8):
        return  # a real texture, not a by-name reference (embedded dictionaries are not handled yet)
    name = u64(b, o + 0x28)
    ref = bytearray(0x50)
    struct.pack_into("<Q", ref, 0, VT_TEXTURE_REF)
    struct.pack_into("<I", ref, 0x10, 0x00260000)
    struct.pack_into("<HH", ref, 0x1C, 1, 1)
    struct.pack_into("<BBHHH", ref, 0x20, 0xFF, 0, 1, 0, 1)
    struct.pack_into("<Q", ref, 0x28, name)
    b[o:o + 0x50] = ref

def write_view(b, o):
    b[o:o + 0x20] = struct.pack("<QQH6s8x", VT_VIEW, 0, 0x14, b"\xff" * 6)

def convert(data, effects):
    magic, version, vflags, pflags = struct.unpack_from("<4sIII", data, 0)
    if magic != b"RSC7" or version != LEGACY_VERSION:
        sys.exit(f"not a legacy fragment (version {version})")
    plain = zlib.decompressobj(-15).decompress(data[16:])
    vsize, psize = block_size(vflags), block_size(pflags)
    if psize:
        sys.exit("legacy physical block not supported yet")
    blk = Block(plain[:vsize], vflags)
    b = blk.data
    report = []

    groups = find_shader_groups(blk)
    geos = find_geometries(blk)
    if any(blk.ptr(u64(b, g + 8)) is not None for g in groups):
        sys.exit("embedded texture dictionary: not supported yet")

    # Declarations, vertex and index buffers.
    decls, done_vb, done_ib = {}, set(), set()
    for g, vb, ib in geos:
        if vb not in done_vb:
            done_vb.add(vb)
            d = blk.ptr(u64(b, vb + 0x30))
            if d not in decls:
                decl, elements, stride = convert_declaration(b, d)
                at = blk.alloc(0x140)
                b = blk.data
                b[at:at + 0x140] = decl
                decls[d] = (at, elements, stride)
            at, elements, stride = decls[d]
            data_ptr, count = u64(b, vb + 0x10), u32(b, vb + 0x18)
            reorder_vertices(b, blk.ptr(data_ptr), count, stride, elements)
            b[vb:vb + 0x80] = bytes(0x80)
            struct.pack_into("<QIHHI4xQ", b, vb, VT_VERTEX_BUFFER, count, stride, 0, 0x00580409, data_ptr)
            struct.pack_into("<QQ", b, vb + 0x30, VIRTUAL + vb + 0x40, VIRTUAL + at)
            write_view(b, vb + 0x40)
        if ib not in done_ib:
            done_ib.add(ib)
            data_ptr, count = u64(b, ib + 0x10), u32(b, ib + 8)
            b[ib:ib + 0x60] = bytes(0x60)
            struct.pack_into("<QIHHI4xQ", b, ib, VT_INDEX_BUFFER, count, 2, 0, 0x0058020A, data_ptr)
            struct.pack_into("<Q", b, ib + 0x30, VIRTUAL + ib + 0x40)
            write_view(b, ib + 0x40)
        # geometry counts (some tools leave them zero)
        if not u32(b, g + 0x5C):
            struct.pack_into("<I", b, g + 0x5C, u32(b, g + 0x58) // 3)
        if not u16(b, g + 0x60):
            struct.pack_into("<H", b, g + 0x60, min(0xFFFF, u32(b, vb + 0x08)))

    # Shaders.
    for g, (arr, shaders) in groups.items():
        for i, s in enumerate(shaders):
            inst = convert_shader(blk, s, effects, report)
            b = blk.data
            struct.pack_into("<Q", b, arr + 8 * i, VIRTUAL + inst)

    # Page map with room for every page (the game fills in the addresses).
    pagemap = blk.alloc(16 + 8 * 128)
    b = blk.data
    struct.pack_into("<QBB", b, pagemap, 0, page_count(blk.flags), 0)
    struct.pack_into("<Q", b, 8, VIRTUAL + pagemap)

    vf = blk.flags | ((ENHANCED_VERSION >> 4) << 28)
    pf = (ENHANCED_VERSION & 0xF) << 28
    assert block_size(blk.flags) == len(b)
    body = zlib.compressobj(9, zlib.DEFLATED, -15)
    packed = body.compress(bytes(b)) + body.flush()
    stats = dict(geometries=len(geos), vertex_buffers=len(done_vb), declarations=len(decls),
                 shaders=sum(len(s) for _, s in groups.values()), pages=page_count(blk.flags), size=len(b))
    return struct.pack("<4sIII", b"RSC7", ENHANCED_VERSION, vf, pf) + packed, stats, report

if __name__ == "__main__":
    effects_path = os.path.join(os.path.dirname(__file__), "..", "research", "effects_6aa45f10.json")
    if "--effects" in sys.argv:
        effects_path = sys.argv[sys.argv.index("--effects") + 1]
    out, stats, report = convert(open(sys.argv[1], "rb").read(), json.load(open(effects_path)))
    open(sys.argv[2], "wb").write(out)
    for line in report:
        print("  warning:", line)
    print(f"{sys.argv[2]}: {stats}, {len(out)} bytes")
