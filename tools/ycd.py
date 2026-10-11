"""Clip dictionary (.ycd) reader, research aid (research/phase0.md §41).
usage: ycd.py <file.ycd> [anim]   (lists clips, animations, tracks; with "anim", dumps each animation's sequences)

Layout found so far (Enhanced = legacy, resource version 46):
  dictionary @0: +0x18 -> animation map, +0x28 -> clip buckets (u16 count, u16 entries at +0x30)
  map: +0x18 -> buckets, +0x20 u16 bucket count, u16 entries; bucket node: u32 hash, +8 -> object, +0x10 -> next
  clip: +0x18 -> name, +0x50 -> animation list (entries 0x18: f32 start, f32 end, f32 rate, +0x10 -> animation),
        +0x58 u16 count, +0x60 f32 duration; +0x40 -> property map
  animation: +0x14 u16 frames, +0x16 u16 frames per chunk, +0x18 f32 duration, +0x38 u32 sequence bytes,
        +0x40 -> sequence pointers (u16 count +0x48), +0x50 -> tracks (u32: u16 bone id, u8 track, u8 type; u16 count +0x58)
"""
import struct, sys, zlib


def block_size(flags):
    base = 0x2000 << (flags & 0xF)
    n = ((flags >> 4) & 1) * 16 + ((flags >> 5) & 3) * 8 + ((flags >> 7) & 0xF) * 4 + ((flags >> 11) & 0x3F) * 2 + ((flags >> 17) & 0x7F)
    size = n * base
    for i, div in enumerate((2, 4, 8, 16)):
        if flags >> (27 - i) & 1:
            size += base // div
    return size


class Ycd:
    def __init__(self, path):
        d = open(path, 'rb').read()
        self.vflags, self.pflags = struct.unpack_from('<II', d, 8)
        self.b = zlib.decompressobj(-15).decompress(d[16:])
        self.vsize = block_size(self.vflags)

    def ptr(self, v):
        if v >> 28 == 5:
            return v - 0x50000000
        if v >> 28 == 6:
            return self.vsize + v - 0x60000000
        return None

    def u8(self, o): return self.b[o]
    def u16(self, o): return struct.unpack_from('<H', self.b, o)[0]
    def u32(self, o): return struct.unpack_from('<I', self.b, o)[0]
    def u64(self, o): return struct.unpack_from('<Q', self.b, o)[0]
    def f32(self, o): return struct.unpack_from('<f', self.b, o)[0]
    def p(self, o): return self.ptr(self.u64(o))

    def cstr(self, o):
        return self.b[o:self.b.index(b'\0', o)].decode('latin1') if o is not None else None

    def map(self, m):
        """[(hash, object offset)] of a map object at m (buckets at +0x18? given by caller)."""
        out = []
        buckets, count = self.p(m), self.u16(m + 8)
        for i in range(count):
            node = self.p(buckets + 8 * i)
            while node is not None:
                out.append((self.u32(node), self.p(node + 8)))
                node = self.p(node + 0x10)
        return out

    def clips(self):
        return self.map(0x28)

    def animations(self):
        return self.map(self.p(0x18) + 0x18)

    def clip(self, c):
        name = self.cstr(self.p(c + 0x18))
        anims = []
        if self.u32(c + 0x10) == 1:  # one animation: +0x50 -> it, +0x58 start, +0x5c end, +0x60 rate
            return name, self.f32(c + 0x5c) - self.f32(c + 0x58), [(self.f32(c + 0x58), self.f32(c + 0x5c), self.f32(c + 0x60), self.p(c + 0x50))]
        lst, n = self.p(c + 0x50), self.u16(c + 0x58)
        if lst is not None:
            for i in range(n):
                e = lst + 0x18 * i
                anims.append((self.f32(e), self.f32(e + 4), self.f32(e + 8), self.p(e + 0x10)))
        return name, self.f32(c + 0x60), anims

    def animation(self, a):
        frames, chunk, duration = self.u16(a + 0x14), self.u16(a + 0x16), self.f32(a + 0x18)
        seqs = [self.p(self.p(a + 0x40) + 8 * i) for i in range(self.u16(a + 0x48))]
        tracks = [self.u32(self.p(a + 0x50) + 4 * i) for i in range(self.u16(a + 0x58))]
        return {'frames': frames, 'chunk': chunk, 'duration': duration, 'seqbytes': self.u32(a + 0x38), 'flags': self.u16(a + 0x10),
                'hash': self.u32(a + 0x1c), 'seqs': seqs, 'tracks': [(t & 0xFFFF, (t >> 16) & 0xFF, t >> 24) for t in tracks]}


if __name__ == '__main__':
    y = Ycd(sys.argv[1])
    print('clips:')
    for h, c in y.clips():
        name, dur, anims = y.clip(c)
        print(f'  {h:08x} {name} dur {dur:.3f} vft {y.u32(c):08x} anims', [(round(s, 3), round(e, 3), round(r, 3), hex(a or 0)) for s, e, r, a in anims])
    print('animations:')
    for h, a in y.animations():
        info = y.animation(a)
        print(f'  {h:08x} @{a:#x} frames {info["frames"]} chunk {info["chunk"]} dur {info["duration"]:.3f} seqbytes {info["seqbytes"]} flags {info["flags"]:#x} seqs {[hex(s) for s in info["seqs"]]}')
        print('    tracks', ' '.join(f'{b:04x}/{t}/{k}' for b, t, k in info['tracks']))
