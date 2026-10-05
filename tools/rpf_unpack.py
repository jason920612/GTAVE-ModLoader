"""Unpack an unencrypted ("OPEN") RPF7 archive into a folder; nested .rpf files are unpacked too
(into a folder of the same name) when they are OPEN as well, otherwise written as they are.
usage: rpf_unpack.py <in.rpf> <out folder>

Layout: see rpf_pack.py. Resource entries are written back with their 16-byte RSC7 header, which is
how the archive stores them, so rpf_pack.py can rebuild the archive from the folder."""
import os, struct, sys, zlib

OPEN = 0x4E45504F

def unpack(data, out):
    magic, count, names_len, enc = struct.unpack_from("<4sIII", data, 0)
    if magic != b"7FPR" or enc != OPEN:
        return False
    entries = data[16:16 + 16 * count]
    names = data[16 + 16 * count:16 + 16 * count + (names_len & 0x0FFFFFFF)]
    shift = (names_len >> 28) & 7
    name_of = lambda off: names[off << shift:].split(b"\0")[0].decode("utf-8")

    def walk(i, path):
        e = entries[16 * i:16 * i + 16]
        if struct.unpack_from("<I", e, 4)[0] == 0x7FFFFF00:
            first, children = struct.unpack_from("<II", e, 8)
            os.makedirs(path, exist_ok=True)
            for c in range(first, first + children):
                walk(c, os.path.join(path, name_of(struct.unpack_from("<H", entries, 16 * c)[0])))
            return
        raw = struct.unpack_from("<Q", e, 0)[0]
        stored = (raw >> 16) & 0xFFFFFF
        offset = ((raw >> 40) & 0x7FFFFF) * 512
        resource = raw >> 63
        size = stored if stored else struct.unpack_from("<I", e, 8)[0]
        blob = data[offset:offset + size]
        if resource and blob[:4] != b"RSC7":
            print(f"  {path}: resource without RSC7 header")
        if stored and not resource:  # compressed binary file: raw deflate
            blob = zlib.decompressobj(-15).decompress(blob)
        if path.lower().endswith(".rpf") and unpack(blob, path):
            return
        with open(path, "wb") as f:
            f.write(blob)
    walk(0, out)
    return True

if __name__ == "__main__":
    if not unpack(open(sys.argv[1], "rb").read(), sys.argv[2]):
        sys.exit("not an OPEN RPF7 archive")
    print("unpacked to", sys.argv[2])
