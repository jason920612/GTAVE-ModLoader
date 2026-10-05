"""For each thread in a minidump, list module-resolved values found on its stack (approximate call stack)."""
import sys, struct
from minidump.minidumpfile import MinidumpFile
mf = MinidumpFile.parse(sys.argv[1]); r = mf.get_reader()
mods = sorted((m.baseaddress, m.size, m.name.split("\\")[-1]) for m in mf.modules.modules)
def where(a):
    for b, s, n in mods:
        if b <= a < b + s: return f"{n}+{a-b:#x}"
    return None
for t in mf.threads.threads:
    try:
        data = r.read(t.Stack.StartOfMemoryRange, min(t.Stack.MemoryLocation.DataSize, 0x3000))
    except Exception as e:
        print(t.ThreadId, "stack unreadable"); continue
    hits = []
    for i in range(0, len(data) - 8, 8):
        w = where(struct.unpack_from("<Q", data, i)[0])
        if w and not w.endswith("+0x0"): hits.append(w)
    mine = [h for h in hits if h.lower().startswith(("version", "hello"))]
    flag = " <== OUR DLL" if mine else ""
    print(f"T{t.ThreadId}{flag}: {hits[:12]}")
