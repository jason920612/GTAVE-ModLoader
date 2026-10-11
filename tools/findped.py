"""Research: find the player ped object from its position bytes (research/phase0.md §42).
usage: findped.py <position hex (12 bytes)> <heading degrees>
For each copy of the position, checks whether it is the translation row of a 4x4 matrix whose three rows above are
orthonormal (a placed entity's matrix) and whether the object around it starts with a pointer into the game image."""
import math, struct, sys
import pymem
sys.path.insert(0, __import__('os').path.dirname(__file__))
import memfind

pm = memfind.pm
base = pymem.process.module_from_name(pm.process_handle, "GTA5_Enhanced.exe").lpBaseOfDll
size = pymem.process.module_from_name(pm.process_handle, "GTA5_Enhanced.exe").SizeOfImage
needle = bytes.fromhex(sys.argv[1])
heading = math.radians(float(sys.argv[2]))

for hit in memfind.find(needle, 200):
    try:
        rows = [struct.unpack('<4f', pm.read_bytes(hit - 0x30 + 0x10 * i, 16)) for i in range(3)]
    except Exception:
        continue
    lens = [math.sqrt(sum(c * c for c in r[:3])) for r in rows]
    if not all(abs(l - 1) < 1e-3 for l in lens):
        continue
    dots = [sum(rows[a][k] * rows[b][k] for k in range(3)) for a, b in ((0, 1), (0, 2), (1, 2))]
    if not all(abs(d) < 1e-3 for d in dots):
        continue
    # Forward row (+0x10 = y axis) against the heading.
    fwd = rows[1]
    h = math.degrees(math.atan2(-fwd[0], fwd[1])) % 360
    print(f'matrix at {hit - 0x30:#x}: rows ok, heading from y row {h:.2f} (want {math.degrees(heading) % 360:.2f})')
    for back in range(0x30, 0x200, 8):
        v = struct.unpack('<Q', pm.read_bytes(hit - back, 8))[0]
        if base <= v < base + size:
            print(f'   pointer into image at -{back:#x}: rva {v - base:#x}')
