"""Dump a resource loaded in the running game back to its file form (research aid).
usage: dump_resource.py <txd|frag|dwd|drawable> <name> <virtual flags hex> <physical flags hex> <out.bin>

The object's page map (+0x08: 8 zero bytes, u8 virtual pages, u8 physical pages, then one
pointer per page) lists where each page of the resource was loaded. Pages are concatenated
in that order (sizes from the block flags, largest class first) and every pointer into a page
is turned back into 0x50000000 (virtual) / 0x60000000 (physical) + file offset.
Writes the plain virtual+physical image (no RSC7 header) and prints what it could not map.
"""
import struct, sys
import pymem

STORES = {"txd": 0x3ED61C8, "frag": 0x3ED6308}

def joaat(s):
    h = 0
    for c in s.lower().encode():
        h = (h + c) & 0xFFFFFFFF
        h = (h + (h << 10)) & 0xFFFFFFFF
        h ^= h >> 6
    h = (h + (h << 3)) & 0xFFFFFFFF
    h ^= h >> 11
    return (h + (h << 15)) & 0xFFFFFFFF

def page_sizes(flags):
    """Page sizes of a block, largest class first (the game's own encoding)."""
    base = 0x2000 << (flags & 0xF)
    sizes = []
    sizes += [base * 16] * ((flags >> 4) & 1)
    sizes += [base * 8] * ((flags >> 5) & 3)
    sizes += [base * 4] * ((flags >> 7) & 0xF)
    sizes += [base * 2] * ((flags >> 11) & 0x3F)
    sizes += [base] * ((flags >> 17) & 0x7F)
    for bit, div in ((0x1000000, 2), (0x2000000, 4), (0x4000000, 8), (0x8000000, 16)):
        if flags & bit:
            sizes.append(base // div)
    return sizes

def find_object(pm, store, name):
    s = pm.process_base.lpBaseOfDll + STORES[store]
    entries, count = pm.read_ulonglong(s + 0x40), pm.read_uint(s + 0x10)
    blob = pm.read_bytes(entries, count * 0x18)
    target = struct.pack("<I", joaat(name))
    for i in range(count):
        e = blob[i * 0x18:(i + 1) * 0x18]
        if e[12:16] == target:
            return struct.unpack_from("<Q", e, 0)[0], i
    return None, None

def main():
    store, name, vflags, pflags, out = sys.argv[1], sys.argv[2], int(sys.argv[3], 16), int(sys.argv[4], 16), sys.argv[5]
    pm = pymem.Pymem("GTA5_Enhanced.exe")
    obj, slot = find_object(pm, store, name)
    if not obj:
        sys.exit(f"{name}: not loaded (slot {slot})")
    pagemap = pm.read_ulonglong(obj + 8)
    vcount, pcount = pm.read_bytes(pagemap + 8, 2)
    vsizes, psizes = page_sizes(vflags), page_sizes(pflags)
    if (vcount, pcount) != (len(vsizes), len(psizes)):
        sys.exit(f"page map says {vcount}+{pcount} pages, flags give {len(vsizes)}+{len(psizes)}")
    addrs = [pm.read_ulonglong(pagemap + 16 + 8 * i) for i in range(vcount + pcount)]
    pages = []  # (address, size, file pointer base)
    off = 0
    for a, size in zip(addrs[:vcount], vsizes):
        pages.append((a, size, 0x50000000 + off)); off += size
    off = 0
    for a, size in zip(addrs[vcount:], psizes):
        pages.append((a, size, 0x60000000 + off)); off += size
    print(f"{name}: object {obj:#x} slot {slot}, pages:", ", ".join(f"{a:#x}+{s:#x}" for a, s, _ in pages))
    if addrs[0] != obj:
        print(f"  note: first page {addrs[0]:#x} != object")

    def unfix(data):
        out = bytearray(data)
        unmapped = 0
        for o in range(0, len(out) - 7, 8):
            v = struct.unpack_from("<Q", out, o)[0]
            if v < 0x10000:
                continue
            for a, size, fb in pages:
                if a <= v < a + size:
                    struct.pack_into("<Q", out, o, fb + (v - a)); break
        return bytes(out)

    image = bytearray()
    for a, size, fb in pages:
        image += unfix(pm.read_bytes(a, size))
    open(out, "wb").write(image)
    print(f"  wrote {len(image):#x} bytes to {out}")

if __name__ == "__main__":
    main()
