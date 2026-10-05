"""Dump the unpacked GTA5_Enhanced.exe image from a running process.

Reads the whole mapped image page-by-page (unreadable pages are zero-filled),
then rewrites section headers so raw offsets == virtual addresses, producing a
PE file that disassemblers can load directly.
usage: dump.py <out.exe>
"""
import ctypes, ctypes.wintypes as w, struct, sys
import pymem

PAGE = 0x1000
out = sys.argv[1] if len(sys.argv) > 1 else "GTA5_Enhanced.dump.exe"

pm = pymem.Pymem("GTA5_Enhanced.exe")
mod = pymem.process.module_from_name(pm.process_handle, "GTA5_Enhanced.exe")
base, size = mod.lpBaseOfDll, mod.SizeOfImage
print(f"base={base:#x} size={size:#x}")

buf = bytearray(size)
bad = 0
for off in range(0, size, PAGE):
    try:
        buf[off:off + PAGE] = pm.read_bytes(base + off, PAGE)
    except Exception:
        bad += 1
print(f"unreadable pages: {bad}")

e_lfanew = struct.unpack_from("<I", buf, 0x3C)[0]
nsec = struct.unpack_from("<H", buf, e_lfanew + 6)[0]
opt_size = struct.unpack_from("<H", buf, e_lfanew + 20)[0]
sec = e_lfanew + 24 + opt_size
for i in range(nsec):
    h = sec + i * 40
    vsize, va = struct.unpack_from("<II", buf, h + 8)
    struct.pack_into("<II", buf, h + 16, (vsize + PAGE - 1) & ~(PAGE - 1), va)
# ImageBase = runtime base so absolute addresses line up with the live process
struct.pack_into("<Q", buf, e_lfanew + 24 + 24, base)
open(out, "wb").write(buf)
print("wrote", out)
