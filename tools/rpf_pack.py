"""Pack a folder into an unencrypted ("OPEN") RPF7 archive, stored uncompressed.
usage: rpf_pack.py <folder> <out.rpf>

Layout as read by the game (research/phase0.md §11):
  header  : 'RPF7', entry count, names length (bits 28-30 = name shift, 0 here), encryption
  entries : 16 bytes each, entry 0 is the root directory
    directory: u32 name offset, u32 0x7FFFFF00, u32 first child index, u32 child count
    file     : u64 name offset | packed size << 16 (0 = stored) | (data offset / 512) << 40,
               u32 size, u32 encrypted (0)
  names   : NUL-terminated, offset 0 is the root's empty name
  data    : each file starts on a 512-byte boundary
Resource files (RSC7, e.g. .ytd/.yft) are not supported yet."""
import os, struct, sys

OPEN = 0x4E45504F

def build(folder):
    # Breadth-first so each directory's children are contiguous; children sorted by name.
    entries, names, name_off = [], bytearray(b"\0"), {"": 0}
    def name(n):
        if n not in name_off:
            name_off[n] = len(names); names.extend(n.encode("utf-8") + b"\0")
        return name_off[n]
    entries.append({"dir": True, "name": "", "path": folder})
    queue = [0]
    while queue:
        i = queue.pop(0)
        path = entries[i]["path"]
        children = sorted(os.listdir(path), key=lambda s: s.lower())
        entries[i]["first"], entries[i]["count"] = len(entries), len(children)
        for c in children:
            full = os.path.join(path, c)
            if os.path.isdir(full):
                entries.append({"dir": True, "name": c.lower(), "path": full}); queue.append(len(entries) - 1)
            else:
                data = open(full, "rb").read()
                if data[:4] == b"RSC7":
                    sys.exit(f"{full}: resource files are not supported yet")
                entries.append({"dir": False, "name": c.lower(), "data": data})
    for e in entries:
        e["noff"] = name(e["name"])
    while len(names) % 16:
        names.append(0)
    toc = 16 + 16 * len(entries) + len(names)
    offset = (toc + 511) // 512 * 512
    blob = bytearray()
    for e in entries:
        if e["dir"]:
            continue
        e["block"] = (offset + len(blob)) // 512
        blob += e["data"]
        blob += b"\0" * (-len(blob) % 512)
    out = bytearray(struct.pack("<4sIII", b"7FPR", len(entries), len(names), OPEN))
    for e in entries:
        if e["dir"]:
            out += struct.pack("<IIII", e["noff"], 0x7FFFFF00, e["first"], e["count"])
        else:
            assert e["noff"] < 1 << 16 and e["block"] < 1 << 23
            out += struct.pack("<QII", e["noff"] | (e["block"] << 40), len(e["data"]), 0)
    out += names
    out += b"\0" * (offset - len(out))
    return bytes(out + blob)

if __name__ == "__main__":
    data = build(sys.argv[1])
    open(sys.argv[2], "wb").write(data)
    print(f"{sys.argv[2]}: {len(data)} bytes")
