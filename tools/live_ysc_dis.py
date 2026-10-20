"""Disassemble a game script loaded in the running game (no files needed; research/phase0.md §22).
usage: live_ysc_dis.py <script_name> [out.txt] | --list   (game running)
Same output as ysc_dis.py; natives are named through the live handler table."""
import json, os, struct, sys
import pymem

here = os.path.dirname(os.path.abspath(__file__))
research = os.path.join(here, "..", "research")
os.environ.setdefault("CROSSMAP", os.path.join(research, "crossmap_oracle.txt"))
sys.path.insert(0, here)
import ysc_dis

pm = pymem.Pymem("GTA5_Enhanced.exe")
base = pymem.process.module_from_name(pm.process_handle, "GTA5_Enhanced.exe").lpBaseOfDll
PROGRAMS = 0x91c0b0 + 0x35b4e78 + 0xD8
NUL = bytes(1)
u32 = lambda a: struct.unpack("<I", pm.read_bytes(a, 4))[0]
u64 = lambda a: struct.unpack("<Q", pm.read_bytes(a, 8))[0]

handler_to_runtime = {}
for line in open(os.path.join(research, "natives_live.csv")):
    h, r = line.strip().split(","); handler_to_runtime[int(r, 16)] = int(h, 16)
runtime_to_public = {}
for line in open(os.path.join(research, "crossmap_oracle.txt")):
    a, b = line.strip().split(",")[:2]; runtime_to_public[int(b, 16)] = int(a, 16)
names = {}
for ns, v in json.load(open(os.path.join(research, "nativedb.json"))).items():
    for h, i in v.items(): names[int(h, 16)] = i["name"]


def name_of(prog):
    return pm.read_bytes(u64(prog + 0x60), 64).split(NUL)[0].decode(errors="replace")


def programs():
    for i in range(256):
        try:
            prog = u64(base + PROGRAMS + i * 8)
            if prog:
                yield prog, name_of(prog)
        except Exception:
            continue


class LiveProg:
    def __init__(s, prog):
        s.codesize, s.nnatives, s.strsize = u32(prog + 0x1C), u32(prog + 0x2C), u32(prog + 0x70)
        s.name = name_of(prog)
        pages = lambda table, size: b"".join(
            pm.read_bytes(u64(u64(prog + table) + 8 * i), min(0x4000, size - i * 0x4000)) for i in range((size + 0x3FFF) // 0x4000))
        s.code = bytearray(pages(0x10, s.codesize))
        s.strings = bytearray(pages(0x68, s.strsize)) if s.strsize else bytearray()
        entries = u64(prog + 0x40)
        s.native_names = []
        for k in range(s.nnatives):
            rva = u64(entries + k * 8) - base
            rt = handler_to_runtime.get(rva)
            pub = runtime_to_public.get(rt, rt)
            s.native_names.append(names.get(pub, "0x%016X" % pub if pub else "rva_%X" % rva))


if __name__ == "__main__":
    if sys.argv[1] == "--list":
        for _, n in programs():
            print(n)
        sys.exit()
    found = [prog for prog, n in programs() if n.lower() == sys.argv[1].lower()]
    if not found:
        sys.exit("script not loaded")
    p = LiveProg(found[0])
    ysc_dis.native_name = lambda prog, i: prog.native_names[i]
    lines = ysc_dis.disasm(p)
    out = open(sys.argv[2], "w", encoding="utf-8") if len(sys.argv) > 2 else sys.stdout
    for i, (a, t) in enumerate(lines):
        if t == "STRING" and i > 0:
            try: t += ' "%s"' % ysc_dis.string_at(p, int(lines[i - 1][1].split()[-1]))
            except Exception: pass
        print(f"{a:6d}: {t}", file=out)
    print(f"{p.name}: {p.codesize} bytes, {p.nnatives} natives", file=sys.stderr)
