"""Reads a game script (.ysc) into a Program that can be changed (code, natives, statics, strings, global pages) and
writes it back as a new resource, laid out from scratch (research/char4-design.md §3.4, experiment U2).
usage: ysc_write.py <in.ysc> <out.ysc>   (round trip: read and write unchanged)"""
import os, struct, sys, zlib
sys.path.insert(0, os.path.dirname(__file__))
from ysc import load, ptr
from convert_ytd import block_size, page_count

CODE_PAGE = 0x4000      # bytes in a code or string page
GLOBAL_PAGE = 0x4000    # slots in a page of global initial values (0x20000 bytes)
VIRTUAL = 0x50000000    # resource pointer base of the virtual block
MASK64 = (1 << 64) - 1


def rotl(v, n):
    n &= 63
    return ((v << n) | (v >> (64 - n))) & MASK64 if n else v


def rotr(v, n):
    return rotl(v, 64 - (n & 63))


class Program:
    def __init__(self, path):
        raw = open(path, 'rb').read()
        magic, self.version, self.vflags, self.pflags = struct.unpack_from('<4sIII', raw, 0)
        assert magic == b'RSC7', path
        x, vs = load(path)
        q = lambda o: struct.unpack_from('<Q', x, o)[0]
        u = lambda o: struct.unpack_from('<I', x, o)[0]
        self.header = bytearray(x[:0x80])  # kept as is except the fields build() writes
        code_size = u(0x1C)
        table = ptr(q(0x10), vs)
        self.code = bytearray()
        for i in range((code_size + CODE_PAGE - 1) // CODE_PAGE):
            p = ptr(q(table + 8 * i), vs)
            self.code += x[p:p + min(CODE_PAGE, code_size - i * CODE_PAGE)]
        natives = ptr(q(0x40), vs)
        # Stored rotated right by (code size + index); kept decoded (the game's hashes).
        self.natives = [rotl(q(natives + 8 * i), code_size + i) for i in range(u(0x2C))] if natives is not None else []
        statics = ptr(q(0x30), vs)
        self.statics = [q(statics + 8 * i) for i in range(u(0x24))] if statics is not None else []
        str_size = u(0x70)
        table = ptr(q(0x68), vs)
        self.strings = bytearray()
        for i in range((str_size + CODE_PAGE - 1) // CODE_PAGE):
            p = ptr(q(table + 8 * i), vs)
            self.strings += x[p:p + min(CODE_PAGE, str_size - i * CODE_PAGE)]
        # Global block owned by the script: header word (block << 18 | slot count), initial values in pages.
        self.globals_word = u(0x28) & 0xFFFFFFFF
        count = self.globals_word & 0x3FFFF
        table = ptr(q(0x38), vs)
        self.globals = []
        if table is not None and count:
            for i in range((count + GLOBAL_PAGE - 1) // GLOBAL_PAGE):
                p = ptr(q(table + 8 * i), vs)
                n = min(GLOBAL_PAGE, count - i * GLOBAL_PAGE)
                self.globals += list(struct.unpack_from('<%dQ' % n, x, p))
        name = ptr(q(0x60), vs)
        self.name = x[name:].split(b'\0')[0].decode('latin1')

    def build(self):
        code_pages = [bytes(self.code[i:i + CODE_PAGE]) for i in range(0, len(self.code), CODE_PAGE)]
        str_pages = [bytes(self.strings[i:i + CODE_PAGE]) for i in range(0, len(self.strings), CODE_PAGE)]
        glob = struct.pack('<%dQ' % len(self.globals), *self.globals)
        glob_pages = [glob[i:i + GLOBAL_PAGE * 8] for i in range(0, len(glob), GLOBAL_PAGE * 8)]
        natives = struct.pack('<%dQ' % len(self.natives), *(rotr(h, len(self.code) + i) for i, h in enumerate(self.natives)))
        statics = struct.pack('<%dQ' % len(self.statics), *self.statics)
        name = self.name.encode('latin1') + b'\0'

        # The root (program header, page map, page tables, name) is the block's first bytes.
        pagemap_at = 0x80
        code_table_at = pagemap_at + 0x20
        str_table_at = code_table_at + 8 * len(code_pages)
        glob_table_at = str_table_at + 8 * len(str_pages)
        name_at = glob_table_at + 8 * len(glob_pages)
        empty_at = (name_at + len(name) + 15) & ~15  # what empty statics and natives point at, as in the game's files
        root_size = empty_at + 16
        items = [('root', root_size, None)]
        items += [(('code', i), len(p), p) for i, p in enumerate(code_pages)]
        items += [(('str', i), len(p), p) for i, p in enumerate(str_pages)]
        items += [(('glob', i), len(p), p) for i, p in enumerate(glob_pages)]
        if statics:
            items.append(('statics', len(statics), statics))
        if natives:
            items.append(('natives', len(natives), natives))

        largest = max(size for _, size, _ in items)
        shift = 0
        while (0x2000 << shift) < largest:
            shift += 1
        while True:
            page = 0x2000 << shift
            free, at = [], {}
            for key, size, _ in [items[0]] + sorted(items[1:], key=lambda it: -it[1]):
                need = (size + 15) & ~15
                for i, f in enumerate(free):
                    if f >= need:
                        at[key] = i * page + (page - f)
                        free[i] -= need
                        break
                else:
                    at[key] = len(free) * page
                    free.append(page - need)
            if len(free) <= 127:
                break
            shift += 1
        vflags = (self.vflags & 0xF0000000) | shift | (len(free) << 17)
        assert block_size(vflags) == len(free) * page and page_count(vflags) == len(free)

        block = bytearray(block_size(vflags))
        for key, size, data in items[1:]:
            block[at[key]:at[key] + size] = data
        vp = lambda offset: VIRTUAL + offset
        block[0:0x80] = self.header
        struct.pack_into('<Q', block, 0x08, vp(pagemap_at))
        struct.pack_into('<Q', block, 0x10, vp(code_table_at))
        struct.pack_into('<I', block, 0x1C, len(self.code))
        struct.pack_into('<I', block, 0x24, len(self.statics))
        struct.pack_into('<I', block, 0x28, self.globals_word)
        struct.pack_into('<I', block, 0x2C, len(self.natives))
        struct.pack_into('<Q', block, 0x30, vp(at['statics'] if statics else empty_at))
        struct.pack_into('<Q', block, 0x38, vp(glob_table_at) if glob_pages else 0)
        struct.pack_into('<Q', block, 0x40, vp(at['natives'] if natives else empty_at))
        struct.pack_into('<Q', block, 0x60, vp(name_at))
        struct.pack_into('<Q', block, 0x68, vp(str_table_at))
        struct.pack_into('<I', block, 0x70, len(self.strings))
        block[pagemap_at:pagemap_at + 0x20] = bytes(0x20)
        block[pagemap_at + 8] = len(free)
        for i in range(len(code_pages)):
            struct.pack_into('<Q', block, code_table_at + 8 * i, vp(at[('code', i)]))
        for i in range(len(str_pages)):
            struct.pack_into('<Q', block, str_table_at + 8 * i, vp(at[('str', i)]))
        for i in range(len(glob_pages)):
            struct.pack_into('<Q', block, glob_table_at + 8 * i, vp(at[('glob', i)]))
        block[name_at:name_at + len(name)] = name

        packer = zlib.compressobj(9, zlib.DEFLATED, -15)
        body = packer.compress(bytes(block)) + packer.flush()
        return struct.pack('<4sIII', b'RSC7', self.version, vflags, self.pflags) + body


def same(a, b):
    """Differences between two programs (empty when equivalent)."""
    diffs = []
    for field in ('code', 'natives', 'statics', 'strings', 'globals', 'globals_word', 'name'):
        if getattr(a, field) != getattr(b, field):
            diffs.append(field)
    for o, n in ((0x18, 4), (0x20, 4), (0x58, 4)):
        if a.header[o:o + n] != b.header[o:o + n]:
            diffs.append('header+%#x' % o)
    return diffs


if __name__ == '__main__':
    p = Program(sys.argv[1])
    data = p.build()
    open(sys.argv[2], 'wb').write(data)
    diffs = same(p, Program(sys.argv[2]))
    print('%s: code %d, natives %d, statics %d, strings %d, globals %d -> %d bytes%s' % (
        p.name, len(p.code), len(p.natives), len(p.statics), len(p.strings), len(p.globals), len(data),
        ', DIFFERS: ' + ', '.join(diffs) if diffs else ', round trip identical'))
