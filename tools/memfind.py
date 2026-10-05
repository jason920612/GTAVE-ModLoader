"""Search the live game's committed private memory for byte patterns.
usage: memfind.py <hex bytes> [max hits]"""
import ctypes, ctypes.wintypes as w, sys, pymem
pm = pymem.Pymem("GTA5_Enhanced.exe")
class MBI(ctypes.Structure):
    _fields_ = [("BaseAddress", ctypes.c_uint64), ("AllocationBase", ctypes.c_uint64), ("AllocationProtect", w.DWORD), ("_a", w.DWORD),
                ("RegionSize", ctypes.c_uint64), ("State", w.DWORD), ("Protect", w.DWORD), ("Type", w.DWORD), ("_b", w.DWORD)]
def regions():
    addr = 0; k32 = ctypes.windll.kernel32; mbi = MBI()
    while k32.VirtualQueryEx(pm.process_handle, ctypes.c_uint64(addr), ctypes.byref(mbi), ctypes.sizeof(mbi)):
        if mbi.State == 0x1000 and mbi.Protect in (0x04, 0x40) and mbi.RegionSize < 0x40000000:
            yield mbi.BaseAddress, mbi.RegionSize
        addr = mbi.BaseAddress + mbi.RegionSize
        if addr >= 0x7FFFFFFF0000: break
def find(needle, limit=50):
    out = []
    for b, s in regions():
        try: data = pm.read_bytes(b, s)
        except Exception: continue
        i = data.find(needle)
        while i != -1:
            out.append(b + i)
            if len(out) >= limit: return out
            i = data.find(needle, i + 1)
    return out
if __name__ == "__main__":
    for a in find(bytes.fromhex(sys.argv[1]), int(sys.argv[2]) if len(sys.argv) > 2 else 50): print(hex(a))
