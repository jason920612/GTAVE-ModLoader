"""Convert a legacy texture dictionary (.ytd, resource version 13) to GTA V Enhanced (version 5).
usage: convert_ytd.py <legacy.ytd> <out.ytd> [--only name,name,...]

Formats (research/phase0.md §12-13, derived from the game and its own files):
  container : RSC7 header {magic, version, virtual flags, physical flags} + raw deflate of
              the virtual block followed by the physical block. Flags encode block sizes.
  dictionary: +0x08 page map, +0x18 usage (1), +0x20 atArray<u32> name hashes (sorted),
              +0x30 atArray<texture*> in hash order. Same in both versions.
  legacy texture (0x90): +0x28 name, +0x50 u16 width, +0x52 height, +0x54 depth, +0x56 row
              pitch, +0x58 D3D format, +0x5D mip count, +0x70 pixel data (physical).
  Enhanced texture (0x80): +0x08 u32 block count, +0x0C u16 block bytes, +0x10 u32 flags,
              +0x18 u16 width, +0x1A height, +0x1C depth, +0x1E dimension (1 = 2D), +0x1F DXGI
              format, +0x20 0xFF, +0x22 mip count, +0x26 1, +0x28 name, +0x30 -> view (+0x58,
              0x28 bytes, filled by the game), +0x38 pixel data, +0x40 u16 usage, +0x42/+0x44.
  Pointers: virtual 0x50000000 + offset, physical 0x60000000 + offset.
"""
import struct, sys, zlib

VIRTUAL, PHYSICAL = 0x50000000, 0x60000000

def joaat(s):
    h = 0
    for c in s.lower().encode():
        h = (h + c) & 0xFFFFFFFF
        h = (h + (h << 10)) & 0xFFFFFFFF
        h ^= h >> 6
    h = (h + (h << 3)) & 0xFFFFFFFF
    h ^= h >> 11
    return (h + (h << 15)) & 0xFFFFFFFF

# --- block sizes (the game's own formula, GTA5_Enhanced.exe +0x1579E0) ----------------------

def block_size(flags):
    base = 0x2000 << (flags & 0xF)
    count = (flags & 0x10) + ((flags >> 2) & 0x18) + ((flags >> 5) & 0x3C) + ((flags >> 10) & 0x7E) + ((flags >> 17) & 0x7F)
    size = count * base
    for bit, div in ((0x8000000, 16), (0x4000000, 8), (0x2000000, 4), (0x1000000, 2)):
        if flags & bit:
            size += base // div
    return size

def page_count(flags):
    """Pages the game allocates for a block (one page-map slot each)."""
    return ((flags >> 4) & 1) + ((flags >> 5) & 3) + ((flags >> 7) & 0xF) + ((flags >> 11) & 0x3F) + ((flags >> 17) & 0x7F) + \
        sum(1 for bit in (0x8000000, 0x4000000, 0x2000000, 0x1000000) if flags & bit)

def encode_flags(size, version_nibble):
    """Smallest encodable block >= size, built from pages of 1x/2x/4x/8x/16x base."""
    if size == 0:
        return version_nibble << 28
    for shift in range(16):
        base = 0x2000 << shift
        units = -(-size // base)
        if units > 16 + 3 * 8 + 15 * 4 + 63 * 2 + 127:
            continue
        n16 = min(units // 16, 1); units -= n16 * 16
        n8 = min(units // 8, 3); units -= n8 * 8
        n4 = min(units // 4, 15); units -= n4 * 4
        n2 = min(units // 2, 63); units -= n2 * 2
        n1 = units
        if n1 > 127:
            continue
        flags = shift | (n16 << 4) | (n8 << 5) | (n4 << 7) | (n2 << 11) | (n1 << 17) | (version_nibble << 28)
        assert block_size(flags) >= size
        return flags
    raise ValueError(f"block of {size:#x} bytes cannot be encoded")

def pack_pages(items, key):
    """Place (item, size) pairs in equal pages; sets item[key] = offset. Returns (block, flags)."""
    largest = max(size for _, size in items)
    shift = 0
    while (0x2000 << shift) < largest:
        shift += 1
    while True:
        page = 0x2000 << shift
        pages = []  # free bytes per page
        for item, size in sorted(items, key=lambda x: -x[1]):
            for i, free in enumerate(pages):
                if free >= size:
                    item[key] = i * page + (page - free)
                    pages[i] -= size
                    break
            else:
                item[key] = len(pages) * page
                pages.append(page - size)
        if len(pages) <= 127:
            break
        shift += 1
    flags = shift | (len(pages) << 17)
    assert block_size(flags) == len(pages) * page and page_count(flags) == len(pages)
    return bytearray(block_size(flags)), flags

# --- pixel formats ---------------------------------------------------------------------------

D3D_TO_DXGI = {  # D3D9 format (FourCC or enum) -> (DXGI format, bytes per 4x4 block or per pixel, is_block)
    0x31545844: (71, 8, True),    # DXT1 -> BC1_UNORM
    0x33545844: (74, 16, True),   # DXT3 -> BC2_UNORM
    0x35545844: (77, 16, True),   # DXT5 -> BC3_UNORM
    0x31495441: (80, 8, True),    # ATI1 -> BC4_UNORM
    0x32495441: (83, 16, True),   # ATI2 -> BC5_UNORM
    0x20374342: (98, 16, True),   # "BC7 " -> BC7_UNORM
    21: (87, 4, False),           # A8R8G8B8 -> B8G8R8A8_UNORM
    22: (88, 4, False),           # X8R8G8B8 -> B8G8R8X8_UNORM
    32: (28, 4, False),           # A8B8G8R8 -> R8G8B8A8_UNORM
    25: (86, 2, False),           # A1R5G5B5 -> B5G5R5A1_UNORM
    28: (65, 1, False),           # A8 -> A8_UNORM
    50: (61, 1, False),           # L8 -> R8_UNORM
}

def mip_bytes(width, height, unit, is_block):
    if is_block:
        return max(1, (width + 3) // 4) * max(1, (height + 3) // 4) * unit
    return width * height * unit

# --- conversion ------------------------------------------------------------------------------

def read_legacy(path):
    data = open(path, "rb").read()
    magic, version, vflags, pflags = struct.unpack_from("<4sIII", data, 0)
    if magic != b"RSC7" or version != 13:
        sys.exit(f"{path}: not a legacy texture dictionary (version {version})")
    plain = zlib.decompressobj(-15).decompress(data[16:])
    vsize, psize = block_size(vflags), block_size(pflags)
    if len(plain) < vsize + psize:
        sys.exit(f"{path}: truncated ({len(plain):#x} < {vsize + psize:#x})")
    return plain[:vsize], plain[vsize:vsize + psize]

def convert(virtual, physical, only=None):
    q = lambda o: struct.unpack_from("<Q", virtual, o)[0]
    v = lambda p: p - VIRTUAL
    count = struct.unpack_from("<H", virtual, 0x38)[0]
    pointers = [q(v(q(0x30)) + 8 * i) for i in range(count)]
    textures = []
    for p in pointers:
        o = v(p)
        name_ptr = q(o + 0x28)
        name = virtual[v(name_ptr):].split(b"\0")[0].decode("ascii", "replace")
        if only and name.lower() not in only:
            continue
        width, height, depth, pitch = struct.unpack_from("<HHHH", virtual, o + 0x50)
        fmt = struct.unpack_from("<I", virtual, o + 0x58)[0]
        mips = virtual[o + 0x5D]
        data_ptr = q(o + 0x70)
        if fmt not in D3D_TO_DXGI:
            sys.exit(f"{name}: unsupported format {fmt:#x}")
        dxgi, unit, is_block = D3D_TO_DXGI[fmt]
        sizes, w, h = [], width, height
        for _ in range(max(1, mips)):
            sizes.append(mip_bytes(w, h, unit, is_block))
            w, h = max(1, w // 2), max(1, h // 2)
        start = data_ptr - PHYSICAL
        pixels = physical[start:start + sum(sizes)]
        textures.append(dict(name=name, width=width, height=height, depth=max(1, depth), mips=max(1, mips),
                             dxgi=dxgi, unit=unit, pixels=pixels))
    textures.sort(key=lambda t: joaat(t["name"]))

    # Physical block. The game loads each page of a block into its own allocation, so a texture
    # must not cross a page boundary: use equal pages (one size class) large enough for the
    # biggest texture and fill them first-fit, largest first. Mips stay back to back.
    for t in textures:
        t["stored"] = (len(t["pixels"]) + 0xFFF) & ~0xFFF
    phys, pflags = pack_pages([(t, t["stored"]) for t in textures], "offset")
    for t in textures:
        phys[t["offset"]:t["offset"] + len(t["pixels"])] = t["pixels"]

    # Virtual block layout.
    n = len(textures)
    off_ptrs = 0x40
    off_hashes = (off_ptrs + 8 * n + 15) & ~15
    off_tex = (off_hashes + 4 * n + 15) & ~15
    off_pagemap = off_tex + 0x80 * n
    # Page counts depend on the final virtual size; one more pass if they change.
    vpages_guess = 1
    for _ in range(4):
        off_names = (off_pagemap + 16 + 8 * (vpages_guess + page_count(pflags)) + 15) & ~15
        names = bytearray()
        for t in textures:
            t["name_off"] = off_names + len(names)
            names += t["name"].encode() + b"\0"
        vsize = off_names + len(names)
        # One page: structures must not cross page boundaries either.
        vshift = 0
        while (0x2000 << vshift) < vsize:
            vshift += 1
        vflags = vshift | (1 << 17)
        if page_count(vflags) == vpages_guess:
            break
        vpages_guess = page_count(vflags)
    virt = bytearray(block_size(vflags))

    struct.pack_into("<QQQQ", virt, 0, 0, VIRTUAL + off_pagemap, 0, 1)
    struct.pack_into("<QHH", virt, 0x20, VIRTUAL + off_hashes, n, n)
    struct.pack_into("<QHH", virt, 0x30, VIRTUAL + off_ptrs, n, n)
    for i, t in enumerate(textures):
        o = off_tex + 0x80 * i
        struct.pack_into("<Q", virt, off_ptrs + 8 * i, VIRTUAL + o)
        struct.pack_into("<I", virt, off_hashes + 4 * i, joaat(t["name"]))
        struct.pack_into("<IHH", virt, o + 0x08, t["stored"] // t["unit"], t["unit"], 0)
        struct.pack_into("<I", virt, o + 0x10, 0x01A70208)
        struct.pack_into("<HHHBB", virt, o + 0x18, t["width"], t["height"], t["depth"], 1, t["dxgi"])
        struct.pack_into("<BBBBBBBB", virt, o + 0x20, 0xFF, 0, t["mips"], 0, 0, 0, 1, 0)
        struct.pack_into("<QQQ", virt, o + 0x28, VIRTUAL + t["name_off"], VIRTUAL + o + 0x58, PHYSICAL + t["offset"])
        usage = 0x216 if t["name"].lower().endswith("_n") else 0x214
        struct.pack_into("<HHH", virt, o + 0x40, usage, 0x80, 2)
    struct.pack_into("<BB", virt, off_pagemap + 8, page_count(vflags), page_count(pflags))
    virt[off_names:off_names + len(names)] = names

    phys_block = bytes(phys) + b"\0" * (block_size(pflags) - len(phys))
    vflags |= 0 << 28   # version 5 = 0x05: virtual nibble 0, physical nibble 5
    pflags |= 5 << 28
    body = zlib.compressobj(9, zlib.DEFLATED, -15)
    packed = body.compress(bytes(virt) + phys_block) + body.flush()
    return struct.pack("<4sIII", b"RSC7", 5, vflags, pflags) + packed, textures

if __name__ == "__main__":
    virtual, physical = read_legacy(sys.argv[1])
    only = None
    if "--only" in sys.argv:
        only = {n.strip().lower() for n in sys.argv[sys.argv.index("--only") + 1].split(",")}
    out, textures = convert(virtual, physical, only)
    open(sys.argv[2], "wb").write(out)
    for t in textures:
        print(f"  {t['name']:32} {t['width']}x{t['height']} mips {t['mips']} dxgi {t['dxgi']}")
    print(f"{sys.argv[2]}: {len(textures)} texture(s), {len(out)} bytes")
