"""Follow unconditional jmps through Arxan-split code and print the straight-line path.
usage: trace.py <dump> <rva> [max_instructions]"""
import sys, capstone
d = open(sys.argv[1], "rb").read()
rva = int(sys.argv[2], 16); limit = int(sys.argv[3]) if len(sys.argv) > 3 else 60
md = capstone.Cs(capstone.CS_ARCH_X86, capstone.CS_MODE_64)
n = 0
while n < limit:
    ins = next(md.disasm(d[rva:rva + 16], rva), None)
    if ins is None: print(f"{rva:#x} <bad>"); break
    n += 1
    if ins.mnemonic == "jmp" and ins.op_str.startswith("0x"):
        rva = int(ins.op_str, 16); continue
    print(f"{ins.address:#09x}  {ins.mnemonic} {ins.op_str}")
    if ins.mnemonic in ("ret", "int3") or (ins.mnemonic == "jmp"): break
    rva += ins.size
