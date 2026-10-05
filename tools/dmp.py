"""Summarise a minidump: exception, faulting module+offset, and the crashing thread's stack hits in modules."""
import sys, struct
from minidump.minidumpfile import MinidumpFile
mf = MinidumpFile.parse(sys.argv[1]); r = mf.get_reader()
mods = sorted((m.baseaddress, m.size, m.name.split("\\")[-1]) for m in mf.modules.modules)
def where(a):
    for b, s, n in mods:
        if b <= a < b + s: return f"{n}+{a-b:#x}"
    return f"{a:#x}"
for e in mf.exception.exception_records:
    rec = e.ExceptionRecord
    print("thread", e.ThreadId, "code", rec.ExceptionCode, "at", where(rec.ExceptionAddress),
          "params", [hex(p) for p in rec.ExceptionInformation[:rec.NumberParameters]])
    ctx = e.ContextObject if hasattr(e, "ContextObject") else None
    # context
    th = [t for t in mf.threads.threads if t.ThreadId == e.ThreadId][0]
    raw = r.read(th.ThreadContext.Rva, th.ThreadContext.DataSize) if False else mf.file_handle if False else None
from minidump.streams import ThreadListStream
# scan crashing thread stack for return addresses inside interesting modules
for e in mf.exception.exception_records:
    th = [t for t in mf.threads.threads if t.ThreadId == e.ThreadId][0]
    sp = th.Stack.StartOfMemoryRange; size = th.Stack.MemoryLocation.DataSize
    data = r.read(sp, min(size, 0x4000))
    hits = []
    for i in range(0, len(data) - 8, 8):
        v = struct.unpack_from("<Q", data, i)[0]
        w = where(v)
        if not w.startswith("0x") and any(k in w.lower() for k in ("gta5", "version", "hello")):
            hits.append(w)
    print("stack refs:", hits[:25])
