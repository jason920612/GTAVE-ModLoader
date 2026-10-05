"""Rebuild a resource's file image from data captured while the game inflated it (research aid).
usage: rebuild_resource.py <txd|frag> <name> <capture dir> <out.bin>

Needs the game running with the resource loaded, and capture.bin/capture.idx written by the
loader's research capture: one record per inflate call (output address, size, data offset), in
call order. One zlib stream fills several pages, switching the output pointer between them, so
each page is rebuilt from the records that wrote into its address range (later ones win).
Page map entries are (page address | log2(size / 0x2000)).
"""
import sys, os
import pymem
sys.path.insert(0, os.path.dirname(__file__))
from dump_resource import find_object

def main():
    store, name, cap, out = sys.argv[1:5]
    records = [tuple(int(x, 16) for x in line.split()) for line in open(os.path.join(cap, "capture.idx"))]
    blob = open(os.path.join(cap, "capture.bin"), "rb").read()
    pm = pymem.Pymem("GTA5_Enhanced.exe")
    obj, slot = find_object(pm, store, name)
    if not obj:
        sys.exit(f"{name} not loaded")
    pmap = pm.read_ulonglong(obj + 8)
    vcount, pcount = pm.read_bytes(pmap + 8, 2)
    image, incomplete = bytearray(), 0
    for i in range(vcount + pcount):
        raw = pm.read_ulonglong(pmap + 16 + 8 * i)
        addr, size = raw & ~0xF, 0x2000 << (raw & 0xF)
        page, covered = bytearray(size), bytearray(size)
        for a, n, o in records:
            lo, hi = max(a, addr), min(a + n, addr + size)
            if lo < hi:
                page[lo - addr:hi - addr] = blob[o + lo - a:o + hi - a]
                covered[lo - addr:hi - addr] = b"\1" * (hi - lo)
        got = sum(covered)
        if got < size:
            incomplete += 1
        print(f"  page {i:2} {'V' if i < vcount else 'P'} {addr:#x} +{size:#x} covered {got:#x}")
        image += page
    open(out, "wb").write(image)
    print(f"{name}: {vcount}+{pcount} pages, {incomplete} incomplete, {len(image):#x} bytes -> {out}")

if __name__ == "__main__":
    main()
