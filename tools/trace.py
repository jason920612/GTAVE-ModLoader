"""Follow unconditional jmps through Arxan-split code and print the straight-line path.
Also follows the "push r; lea r, [rip + x]; xchg [rsp], r; ret" jump pattern.
usage: trace.py <dump> <rva> [max_instructions]"""
import re, sys, capstone
d = open(sys.argv[1], "rb").read()
rva = int(sys.argv[2], 16); limit = int(sys.argv[3]) if len(sys.argv) > 3 else 60
md = capstone.Cs(capstone.CS_ARCH_X86, capstone.CS_MODE_64)


def at(a):
    return next(md.disasm(d[a:a + 16], a), None)


n = 0
while n < limit:
    ins = at(rva)
    if ins is None: print(f"{rva:#x} <bad>"); break
    n += 1
    if ins.mnemonic == "jmp" and ins.op_str.startswith("0x"):
        rva = int(ins.op_str, 16); continue
    # push reg; lea reg, [rip +/- x]; xchg qword ptr [rsp], reg; ret  ==  jmp target
    if ins.mnemonic == "push":
        a, b, c = at(ins.address + ins.size), None, None
        if a is not None and a.mnemonic == "lea" and "rip" in a.op_str:
            b = at(a.address + a.size)
            c = at(b.address + b.size) if b is not None else None
            m = re.search(r"\[rip ([+-]) (0x[0-9a-f]+|\d+)\]", a.op_str)
            # ... or "xchg; lea rsp, [rsp + 8]; jmp qword ptr [rsp - 8]"
            e = at(c.address + c.size) if c is not None and c.mnemonic == "lea" and "rsp" in c.op_str else None
            jumps = (c is not None and c.mnemonic == "ret") or (e is not None and e.mnemonic == "jmp" and "rsp - 8" in e.op_str)
            if m and b is not None and b.mnemonic == "xchg" and jumps:
                disp = int(m.group(2), 0)
                rva = a.address + a.size + (disp if m.group(1) == "+" else -disp)
                continue
    print(f"{ins.address:#09x}  {ins.mnemonic} {ins.op_str}")
    if ins.mnemonic in ("ret", "int3") or (ins.mnemonic == "jmp"): break
    rva += ins.size
