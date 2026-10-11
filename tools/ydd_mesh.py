"""Read skinned meshes from a GTA V Enhanced drawable resource (.ydd / .ydr / .yft, research aid).
usage: ydd_mesh.py <file> [--dump]

Container (research/phase0.md §12): 16-byte RSC7 header {magic, version, virtual flags, physical flags} and a raw
deflate stream of the virtual block followed by the physical block. Pointers are 0x5xxxxxxx (virtual) and
0x6xxxxxxx (physical) + offset. Geometries are found the way the converter does (§15-16): a geometry has the vertex
buffer at +0x18 and the index buffer at +0x38. Enhanced buffers: +0x08 u32 count, +0x0C u16 stride (low 12 bits),
+0x18 data, vertex buffers +0x38 declaration (u32 offsets[52], u8 strides[36] @0xD0, u8 DXGI formats[36] @0x104;
slots: 0 position, 16 blend weights, 20 blend indices).
"""
import struct
import sys
import zlib

sys.path.insert(0, __import__("os").path.dirname(__file__))
from dump_resource import page_sizes  # noqa: E402


class Resource:
    def __init__(self, path):
        raw = open(path, "rb").read()
        magic, self.version, vflags, pflags = struct.unpack_from("<4I", raw)
        if magic != 0x37435352:
            raise ValueError("not an RSC7 resource")
        data = zlib.decompress(raw[16:], -15)
        vsize, psize = sum(page_sizes(vflags)), sum(page_sizes(pflags))
        self.v, self.p = data[:vsize], data[vsize:vsize + psize]

    def ptr(self, value):
        """(block, offset) of a pointer, or None."""
        if value >> 28 == 5 and (value & 0x0FFFFFFF) < len(self.v):
            return self.v, value & 0x0FFFFFFF
        if value >> 28 == 6 and (value & 0x0FFFFFFF) < len(self.p):
            return self.p, value & 0x0FFFFFFF
        return None

    def u64(self, o):
        return struct.unpack_from("<Q", self.v, o)[0] if o + 8 <= len(self.v) else 0


def vertex_buffer(res, o):
    """A vertex buffer at virtual offset o: (count, stride, data (block, offset), declaration offset) or None."""
    v = res.v
    if o + 0x40 > len(v):
        return None
    count, stride = struct.unpack_from("<IH", v, o + 8)
    stride &= 0xFFF
    data, decl = res.ptr(res.u64(o + 0x18)), res.ptr(res.u64(o + 0x38))
    if not data or not decl or decl[0] is not v or not count or not stride:
        return None
    if decl[1] + 0x140 > len(v) or v[decl[1] + 0x104] == 0:
        return None
    return count, stride, data, decl[1]


def geometries(res):
    v = res.v
    out = []
    for o in range(0, len(v) - 0xA0, 16):
        vbp, ibp = res.ptr(res.u64(o + 0x18)), res.ptr(res.u64(o + 0x38))
        if not vbp or not ibp or vbp[0] is not v or ibp[0] is not v:
            continue
        vb = vertex_buffer(res, vbp[1])
        if not vb:
            continue
        icount = struct.unpack_from("<I", v, ibp[1] + 8)[0]
        idata = res.ptr(res.u64(ibp[1] + 0x18))
        if not idata or not icount:
            continue
        # Geometry +0x68: bone ids (u16, count at +0x72) mapping blend indices to skeleton bone ids (tags).
        bones = []
        bp, bcount = res.ptr(res.u64(o + 0x68)), struct.unpack_from("<H", v, o + 0x72)[0]
        if bp and bcount:
            bones = list(struct.unpack_from(f"<{bcount}H", bp[0], bp[1]))
        out.append({"at": o, "vb": vbp[1], "ib": ibp[1], "count": vb[0], "stride": vb[1], "data": vb[2], "decl": vb[3],
                    "icount": icount, "idata": idata, "bones": bones})
    return out


def slot(res, decl, k):
    """(offset in vertex, DXGI format) of declaration slot k, or None when absent."""
    fmt = res.v[decl + 0x104 + k]
    if not fmt:
        return None
    return struct.unpack_from("<I", res.v, decl + 4 * k)[0], fmt


def vertices(res, g):
    """[(position, [(index, weight) x4])] of a geometry."""
    block, base = g["data"]
    pos, wts, idx = slot(res, g["decl"], 0), slot(res, g["decl"], 16), slot(res, g["decl"], 20)
    out = []
    for i in range(g["count"]):
        b = base + i * g["stride"]
        p = struct.unpack_from("<3f", block, b + pos[0])
        w = list(block[b + wts[0]:b + wts[0] + 4]) if wts else [255, 0, 0, 0]
        j = list(block[b + idx[0]:b + idx[0] + 4]) if idx else [0, 0, 0, 0]
        out.append((p, list(zip(j, w))))
    return out


def main():
    res = Resource(sys.argv[1])
    gs = geometries(res)
    print(f"version {res.version}, virtual {len(res.v):#x}, physical {len(res.p):#x}, {len(gs)} geometr(ies)")
    for g in gs:
        fmts = {k: slot(res, g["decl"], k) for k in range(36) if slot(res, g["decl"], k)}
        print(f"geometry @{g['at']:#x}: {g['count']} vertices stride {g['stride']:#x}, {g['icount']} indices, slots {fmts}")
        print(f"   bone ids ({len(g['bones'])}): {[hex(b) for b in g['bones']]}")
        if "--dump" in sys.argv:
            hdr = res.v[g["at"]:g["at"] + 0xA0]
            for r in range(0, 0xA0, 16):
                print(f"   +{r:02x}: " + " ".join(f"{struct.unpack_from('<Q', hdr, r + k)[0]:016x}" for k in (0, 8)))


if __name__ == "__main__":
    main()
