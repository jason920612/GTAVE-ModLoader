"""Read script globals of the running game (research/phase0.md §22: block table +0x3ED15A8, block = index >> 18,
offset = index & 0x3FFFF, 8 bytes each; scripts use the low 32 bits).
usage: globals.py <index> [count]            print count globals as int (and float)
       globals.py watch <index> <count> [s]  print the ones that change, every 0.1 s for s seconds (default 10)"""
import struct, sys, time
import pymem

pm = pymem.Pymem("GTA5_Enhanced.exe")
base = pymem.process.module_from_name(pm.process_handle, "GTA5_Enhanced.exe").lpBaseOfDll
BLOCKS = 0x3ED15A8


def addr(i):
    block = struct.unpack("<Q", pm.read_bytes(base + BLOCKS + (i >> 18) * 8, 8))[0]
    return block + (i & 0x3FFFF) * 8


def read(i, n):
    raw = pm.read_bytes(addr(i), n * 8)
    return [struct.unpack_from("<i", raw, k * 8)[0] for k in range(n)], [struct.unpack_from("<f", raw, k * 8)[0] for k in range(n)]


if __name__ == "__main__":
    if sys.argv[1] == "watch":
        i, n = int(sys.argv[2]), int(sys.argv[3])
        end = time.time() + (float(sys.argv[4]) if len(sys.argv) > 4 else 10)
        last, _ = read(i, n)
        while time.time() < end:
            now, _ = read(i, n)
            for k in range(n):
                if now[k] != last[k]:
                    print(f"{time.time():.2f} +{k}: {last[k]} -> {now[k]}", flush=True)
            last = now
            time.sleep(0.1)
    else:
        i = int(sys.argv[1]); n = int(sys.argv[2]) if len(sys.argv) > 2 else 1
        ints, floats = read(i, n)
        for k in range(n):
            print(f"+{k}: {ints[k]}  ({floats[k]:g})")
