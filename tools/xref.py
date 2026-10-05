"""Find rip-relative references (lea/mov) to a target RVA in the dump's code sections.
usage: xref.py <dump> <target_rva> [...]"""
import struct, sys
d = open(sys.argv[1], "rb").read()
targets = {int(t, 16) for t in sys.argv[2:]}
for lo, hi in ((0x1000, 0x1000 + 0x2479400), (0x5123000, 0x5123000 + 0xa22200)):
    for p in range(lo, hi - 4):
        disp = struct.unpack_from("<i", d, p)[0]
        tgt = p + 4 + disp
        if tgt in targets and d[p - 2] in (0x8d, 0x8b) :  # lea/mov r, [rip+disp]
            print(f"ref to {tgt:#x} at {p-3:#x}")
