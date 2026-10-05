"""Find the function (from .pdata) containing an RVA. usage: func.py <dump> <rva> [...]"""
import sys, struct, pefile
pe = pefile.PE(sys.argv[1], fast_load=True)
d = open(sys.argv[1], "rb").read()
exc = pe.OPTIONAL_HEADER.DATA_DIRECTORY[3]
entries = [struct.unpack_from("<III", d, exc.VirtualAddress + i * 12) for i in range(exc.Size // 12)]
entries.sort()
import bisect
starts = [e[0] for e in entries]
for a in sys.argv[2:]:
    rva = int(a, 16)
    i = bisect.bisect_right(starts, rva) - 1
    b, e, u = entries[i]
    print(f"{rva:#x}: function {b:#x}-{e:#x}" if b <= rva < e else f"{rva:#x}: not in a .pdata function (nearest {b:#x}-{e:#x})")
