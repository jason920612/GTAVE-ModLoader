"""Decode the live native registration table: prints count and writes hash,handler_rva lines.
Layout (from InitNativeTables @ RVA 0x91bf20):
  table: 256 node pointers indexed by low byte of hash (RVA TABLE_RVA)
  node+0x00..0x0c : obfuscated next pointer
  node+0x10       : 7 handler pointers
  node+0x48..0x4c : obfuscated entry count
  node+0x54+i*16  : obfuscated 64-bit hash"""
import struct, sys, pymem
TABLE_RVA = 0x91bf40 + 0x35b8ce0
pm = pymem.Pymem("GTA5_Enhanced.exe")
base = pymem.process.module_from_name(pm.process_handle, "GTA5_Enhanced.exe").lpBaseOfDll
u32 = lambda a: struct.unpack("<I", pm.read_bytes(a, 4))[0]
u64 = lambda a: struct.unpack("<Q", pm.read_bytes(a, 8))[0]
M = 0xFFFFFFFF
out = []
for b in range(256):
    node = u64(base + TABLE_RVA + b * 8)
    while node:
        cnt = (u32(node + 0x48) ^ u32(node + 0x4c) ^ ((node + 0x48) & M))
        for i in range(cnt):
            h = node + 0x54 + i * 16
            esi = u32(h + 8) ^ (h & M)
            lo = u32(h) ^ esi; hi = esi ^ u32(h + 4)
            out.append(((hi << 32) | lo, u64(node + 0x10 + i * 8) - base))
        e = u32(node + 8) ^ (node & M)
        nxt_lo = u32(node) ^ e; nxt_hi = e ^ u32(node + 4)
        node = (nxt_hi << 32) | nxt_lo
print("natives:", len(out))
open(sys.argv[1] if len(sys.argv) > 1 else "research/natives_live.csv", "w").write("".join(f"{h:#018x},{r:#x}\n" for h, r in out))
known = {"WAIT": 0x4EDE34FBADD967A6, "PLAYER_PED_ID": 0xD80958FC74E988A6, "GET_ENTITY_COORDS": 0x3FEF770D40960D5A,
         "VDIST": 0x2A488C176D52CCA5, "NETWORK_IS_SESSION_STARTED": 0x9DE624D2FC4B603F}
hs = {h for h, _ in out}
for k, v in known.items(): print(f"{k:28} {v:#018x} present={v in hs}")
