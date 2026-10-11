"""Research: list pointer paths from the player entity to its skeleton (research/phase0.md §42).
usage: skelpath.py <position hex (12 bytes)>
Entity: position at +0x90 (matrix at +0x60), vtable at +0. Skeleton: *(skeleton) = skeleton data, whose first
qword is the skeleton-data vtable (taken from the game image, RVA SKELDATA_VFT)."""
import struct, sys
import pymem
sys.path.insert(0, __import__('os').path.dirname(__file__))
import memfind

SKELDATA_VFT_HINT = None
pm = memfind.pm
mod = pymem.process.module_from_name(pm.process_handle, "GTA5_Enhanced.exe")
base, size = mod.lpBaseOfDll, mod.SizeOfImage
q = lambda a: struct.unpack('<Q', pm.read_bytes(a, 8))[0]
valid = lambda p: 0x10000000000 < p < 0x7ff000000000


def is_skeleton(s):
    try:
        sd = q(s)
        if not valid(sd):
            return False
        vft = q(sd)
        if not (base <= vft < base + size):
            return False
        bones = struct.unpack('<H', pm.read_bytes(sd + 0x18, 2))[0]
        arr = q(sd + 0x20)
        name = q(arr + 0x38) if valid(arr) else 0
        return 20 < bones < 1000 and valid(name) and pm.read_bytes(name, 4) == b'SKEL' and q(s + 0x08) and valid(q(s + 0x10))
    except Exception:
        return False


for hit in memfind.find(bytes.fromhex(sys.argv[1]), 100):
    ent = hit - 0x90
    try:
        rows = [struct.unpack('<3f', pm.read_bytes(ent + 0x60 + 0x10 * k, 12)) for k in range(3)]
    except Exception:
        continue
    if not all(abs(sum(c * c for c in r) - 1) < 1e-3 for r in rows):
        continue
    print(f'entity {ent:#x} (+0 {q(ent):#x})')
    data = pm.read_bytes(ent, 0x1800)
    for i in range(0, len(data), 8):
        p = struct.unpack_from('<Q', data, i)[0]
        if not valid(p):
            continue
        if is_skeleton(p):
            print(f'   entity+{i:#x} -> skeleton {p:#x}')
        try:
            d2 = pm.read_bytes(p, 0x400)
        except Exception:
            continue
        for j in range(0, len(d2), 8):
            s = struct.unpack_from('<Q', d2, j)[0]
            if valid(s) and is_skeleton(s):
                print(f'   entity+{i:#x} -> +{j:#x} -> skeleton {s:#x} (bones {struct.unpack("<H", pm.read_bytes(q(s) + 0x18, 2))[0]})')
