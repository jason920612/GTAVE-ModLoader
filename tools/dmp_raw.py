"""Minimal minidump reader (works on crashpad dumps): exception code/address and module.
usage: dmp_raw.py <file.dmp>"""
import struct, sys
d = open(sys.argv[1], "rb").read()
sig, ver, nstreams, diroff = struct.unpack_from("<IIII", d, 0)
streams = {}
for i in range(nstreams):
    t, size, rva = struct.unpack_from("<III", d, diroff + 12 * i)
    streams[t] = (size, rva)
mods = []
if 4 in streams:
    size, rva = streams[4]
    n = struct.unpack_from("<I", d, rva)[0]
    for i in range(n):
        base, sz = struct.unpack_from("<QI", d, rva + 4 + 108 * i)
        name_rva = struct.unpack_from("<I", d, rva + 4 + 108 * i + 20)[0]
        ln = struct.unpack_from("<I", d, name_rva)[0]
        mods.append((base, sz, d[name_rva + 4:name_rva + 4 + ln].decode("utf-16le").split("\\")[-1]))
def where(a):
    for b, s, n in mods:
        if b <= a < b + s:
            return f"{n}+{a - b:#x}"
    return hex(a)
if 6 in streams:
    size, rva = streams[6]
    tid = struct.unpack_from("<I", d, rva)[0]
    code, flags, rec, addr = struct.unpack_from("<IIQQ", d, rva + 8)
    print(f"thread {tid} code {code:#x} at {where(addr)}")

# Stack of the faulting thread: module-resolved values found on it.
def memory_at(addr, size):
    if 9 in streams:
        _, rva = streams[9]
        n, base_rva = struct.unpack_from("<QQ", d, rva)
        off = base_rva
        for i in range(n):
            start, sz = struct.unpack_from("<QQ", d, rva + 16 + 16 * i)
            if start <= addr < start + sz:
                return d[off + addr - start: off + min(addr - start + size, sz)]
            off += sz
    if 5 in streams:
        _, rva = streams[5]
        n = struct.unpack_from("<I", d, rva)[0]
        for i in range(n):
            start, sz, mrva = struct.unpack_from("<QII", d, rva + 4 + 16 * i)
            if start <= addr < start + sz:
                return d[mrva + addr - start: mrva + min(addr - start + size, sz)]
    return b""
if 6 in streams and 3 in streams:
    _, rva = streams[3]
    n = struct.unpack_from("<I", d, rva)[0]
    for i in range(n):
        t = struct.unpack_from("<I", d, rva + 4 + 48 * i)[0]
        if t != tid:
            continue
        stack_start, stack_size, stack_rva = struct.unpack_from("<QII", d, rva + 4 + 48 * i + 24)
        ctx_size, ctx_rva = struct.unpack_from("<II", d, rva + 4 + 48 * i + 40)
        rsp = struct.unpack_from("<Q", d, ctx_rva + 0x98)[0]
        regs = {r: struct.unpack_from("<Q", d, ctx_rva + 0x78 + 8 * k)[0] for k, r in enumerate(["rax","rcx","rdx","rbx","rsp","rbp","rsi","rdi"])}
        print("regs", {k: hex(v) for k, v in regs.items()})
        mem = d[stack_rva + (rsp - stack_start): stack_rva + stack_size]
        hits = [where(v) for (v,) in struct.iter_unpack("<Q", mem[: len(mem) // 8 * 8]) if any(b <= v < b + s for b, s, _ in mods)]
        print("stack:", hits[:30])

# Context captured at the exception itself.
if 6 in streams:
    _, rva = streams[6]
    csize, crva = struct.unpack_from("<II", d, rva + 8 + 152)
    names = ["rax","rcx","rdx","rbx","rsp","rbp","rsi","rdi","r8","r9","r10","r11","r12","r13","r14","r15"]
    regs = {r: struct.unpack_from("<Q", d, crva + 0x78 + 8 * k)[0] for k, r in enumerate(names)}
    rip = struct.unpack_from("<Q", d, crva + 0xF8)[0]
    print("exception rip", where(rip), {k: hex(v) for k, v in regs.items()})
    mem = memory_at(regs["rsp"], 0x800)
    print("exception stack:", [where(v) for (v,) in struct.iter_unpack("<Q", mem[: len(mem) // 8 * 8]) if any(b <= v < b + s for b, s, _ in mods)][:24])
