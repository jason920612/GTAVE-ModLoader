"""Lists the engine's per-character stat objects (research/char4-design.md §3.7, slot alias): the builder at 0x1eb44d0
fills objects of five stat hashes {SP0, SP1, SP2, MP0, MP1}, most through calls to 0x1ea54c0 (object in rcx, hashes in
[rsp+0x30..0x50]) and a few with direct stores. Names are found by joaat against the SP stats definitions.
usage: char4_statobjs.py <dump.exe> <out.json> <spstatssetup.xml> [...]"""
import json, re, struct, sys
import capstone
sys.path.insert(0, __import__('os').path.dirname(__import__('os').path.abspath(__file__)))
from convert_ytd import joaat

BUILDER, SETTER = 0x1eb44d0, 0x1ea54c0


def objects(dump):
    md = capstone.Cs(capstone.CS_ARCH_X86, capstone.CS_MODE_64)
    stores, stack, rcx, objs = {}, {}, None, {}
    for ins in md.disasm(dump[BUILDER:BUILDER + 0x20000], BUILDER):
        b = ins.bytes
        if b[:2] == b'\xc7\x05':  # mov dword [rip+d], imm
            disp, imm = struct.unpack_from('<iI', b, 2)
            stores[ins.address + ins.size + disp] = imm
        elif b[:3] == b'\xc7\x44\x24':  # mov dword [rsp+d8], imm
            stack[b[3]] = struct.unpack_from('<I', b, 4)[0]
        elif ins.mnemonic == 'lea' and ins.op_str.startswith('rcx, [rip'):
            rcx = ins.address + ins.size + struct.unpack_from('<i', b, 3)[0]
        elif ins.mnemonic == 'call' and ins.op_str == hex(SETTER):
            objs[rcx] = [stack[off] for off in (0x30, 0x38, 0x40, 0x48, 0x50)]
        elif ins.mnemonic == 'ret':
            break
    addrs = sorted(stores)
    i, stray = 0, []
    while i < len(addrs):  # direct stores: runs of five consecutive dwords
        a = addrs[i]
        if all(a + 4 * k in stores for k in range(5)):
            objs[a] = [stores[a + 4 * k] for k in range(5)]
            i += 5
        else:
            stray.append(a)
            i += 1
    return objs, stray


def main():
    dump = open(sys.argv[1], 'rb').read()
    objs, stray = objects(dump)
    names = set()
    for f in sys.argv[3:]:
        for n in re.findall(r'Name="([^"]+)"', open(f, encoding='utf-8', errors='replace').read()):
            m = re.match(r'(?:SP[0-2]|MP[0-1])_(.+)', n)
            names.add(m.group(1) if m else n)
    by_sp0 = {joaat('SP0_' + n): n for n in names}
    out = []
    for addr in sorted(objs):
        h = objs[addr]
        n = by_sp0.get(h[0])
        ok = n is not None and joaat('SP1_' + n) == h[1] and joaat('SP2_' + n) == h[2]
        out.append({'rva': addr, 'hashes': h, 'name': n if ok else None})
    json.dump(out, open(sys.argv[2], 'w'), indent=0)
    print('%d objects, %d named, %d stray dwords %s' % (len(out), sum(1 for o in out if o['name']), len(stray), ' '.join(hex(a) for a in stray)))


if __name__ == '__main__':
    main()
