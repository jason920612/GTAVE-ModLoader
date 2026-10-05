"""Disassemble N instructions at an RVA of the dump. usage: disasm.py <dump> <rva> [count]"""
import sys, capstone, pefile
d = open(sys.argv[1], "rb").read()
base = pefile.PE(sys.argv[1], fast_load=True).OPTIONAL_HEADER.ImageBase
rva = int(sys.argv[2], 16); n = int(sys.argv[3]) if len(sys.argv) > 3 else 40
md = capstone.Cs(capstone.CS_ARCH_X86, capstone.CS_MODE_64)
for i, ins in enumerate(md.disasm(d[rva:rva + n * 15], rva)):
    if i >= n: break
    print(f"{ins.address:#09x}  {ins.bytes.hex():<24} {ins.mnemonic} {ins.op_str}")
