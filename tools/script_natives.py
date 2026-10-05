"""List the natives used by a loaded game script (live process).
usage: script_natives.py <script_name>"""
import json, struct, sys, pymem
pm = pymem.Pymem("GTA5_Enhanced.exe")
base = pymem.process.module_from_name(pm.process_handle, "GTA5_Enhanced.exe").lpBaseOfDll
PROGRAMS = 0x91c0b0 + 0x35b4e78 + 0xD8
u32 = lambda a: struct.unpack("<I", pm.read_bytes(a, 4))[0]
u64 = lambda a: struct.unpack("<Q", pm.read_bytes(a, 8))[0]
handler_to_runtime = {}
for line in open("research/natives_live.csv"):
    h, r = line.strip().split(","); handler_to_runtime[int(r, 16)] = int(h, 16)
runtime_to_public = {}
for line in open("research/crossmap_oracle.txt"):
    a, b = line.strip().split(","); runtime_to_public[int(b, 16)] = int(a, 16)
names = {}
for ns, v in json.load(open("research/nativedb.json")).items():
    for h, i in v.items(): names[int(h, 16)] = f"{ns}::{i['name']}"
want = sys.argv[1].lower()
for i in range(256):
    try:
        prog = u64(base + PROGRAMS + i * 8)
        if not prog: continue
        name = pm.read_bytes(u64(prog + 0x60), 64).split(b"\0")[0].decode(errors="replace")
    except Exception:
        continue
    if name.lower() != want: continue
    count, entries = u32(prog + 0x2C), u64(prog + 0x40)
    print(f"{name}: {count} natives")
    for k in range(count):
        rva = u64(entries + k * 8) - base
        rt = handler_to_runtime.get(rva)
        pub = runtime_to_public.get(rt, rt)
        print(f"  {names.get(pub, hex(pub) if pub else hex(rva))}")
    break
else:
    print("script not loaded")
