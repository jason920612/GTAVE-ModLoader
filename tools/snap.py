"""Snapshot / diff the game's .data section. usage: snap.py save <file> | diff <a> <b>"""
import sys, struct, pymem
if sys.argv[1] == "save":
    pm = pymem.Pymem("GTA5_Enhanced.exe")
    base = pymem.process.module_from_name(pm.process_handle, "GTA5_Enhanced.exe").lpBaseOfDll
    lo, size = 0x286a000, 0x2741428
    data = bytearray()
    for off in range(0, size, 0x100000):
        n = min(0x100000, size - off)
        try: data += pm.read_bytes(base + lo + off, n)
        except Exception: data += b"\0" * n
    open(sys.argv[2], "wb").write(data); print("saved", len(data))
else:
    a, b = open(sys.argv[2], "rb").read(), open(sys.argv[3], "rb").read()
    for off in range(0, min(len(a), len(b)) - 4, 4):
        x, y = struct.unpack_from("<I", a, off)[0], struct.unpack_from("<I", b, off)[0]
        if x != y and x in (0, 1) and y in (0, 1):
            print(f"rva {0x286a000 + off:#x}: {x} -> {y}")
