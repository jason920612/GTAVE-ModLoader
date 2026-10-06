"""Learn legacy texture parameter name -> Enhanced texture name, from resources that exist in both versions.
usage: learn_texture_names.py <Enhanced files dir> <legacy files dir> <out.json>
Pairs a legacy shader's texture parameters with the game's own Enhanced conversion of the same shader through the
texture each one references (by name; only names used once on both sides). Files come from the loader's
"extractall" research command (ModLoader\debug_convert.txt). The result feeds kTextureNames in
src/loader/convert/yft.cpp (research/phase0.md §18)."""
import collections, json, os, struct, sys, zlib

def load(path):
    d = open(path, "rb").read()
    if d[:4] != b"RSC7":
        return None, 0
    try:
        return zlib.decompressobj(-15).decompress(d[16:]), struct.unpack_from("<I", d, 4)[0]
    except zlib.error:
        return None, 0

def q(b, o): return struct.unpack_from("<Q", b, o)[0] if o + 8 <= len(b) else 0
def u32(b, o): return struct.unpack_from("<I", b, o)[0] if o + 4 <= len(b) else 0
def u16(b, o): return struct.unpack_from("<H", b, o)[0] if o + 2 <= len(b) else 0
def ptr(b, p): return p - 0x50000000 if 0x50000000 <= p < 0x50000000 + len(b) else None
def cstr(b, o):
    if o is None: return None
    e = b.find(b"\0", o)
    return b[o:e].decode("latin1") if e > o else None

def shader_groups(b, legacy):
    groups = []
    for o in range(0, len(b) - 0x20, 16):
        arr = ptr(b, q(b, o + 0x10)); n, cap = u16(b, o + 0x18), u16(b, o + 0x1A)
        if arr is None or not 0 < n <= cap <= 1024:
            continue
        shaders = [ptr(b, q(b, arr + 8 * i)) for i in range(n)]
        if legacy:
            ok = all(s is not None and ptr(b, q(b, s)) is not None and b[s + 0x13] == 0x80 for s in shaders)
        else:
            ok = all(s is not None and u32(b, s + 4) == 0x6D657461 for s in shaders)
        if ok:
            groups.append(shaders)
    return groups

def texname(b, p):
    o = ptr(b, p)
    return cstr(b, ptr(b, q(b, o + 0x28))) if o is not None else None

def legacy_textures(b, s):
    params = ptr(b, q(b, s)); c = b[s + 0x10]
    types = [b[params + 16 * i] for i in range(c)]
    hashes = params + 16 * c + sum(16 * t for t in types)
    return u32(b, s + 8), [(u32(b, hashes + 4 * i), texname(b, q(b, params + 16 * i + 8))) for i in range(c) if types[i] == 0]

def enhanced_textures(b, s):
    meta = ptr(b, q(b, s + 0x20)); tex = ptr(b, q(b, s + 0x10))
    count = b[meta + 1]
    out = []
    for k in range(b[meta + 4]):
        h, info = u32(b, meta + 8 + 8 * k), u32(b, meta + 12 + 8 * k)
        if info & 3 == 0:
            out.append((h, texname(b, q(b, tex + 8 * ((info >> 2) & 63)))))
    return u32(b, s), out

pairs = collections.defaultdict(collections.Counter)  # (effect, legacy) -> Counter(enhanced)
anyeffect = collections.defaultdict(collections.Counter)
enh_dir, leg_dir = sys.argv[1], sys.argv[2]
files = 0
for name in sorted(os.listdir(enh_dir)):
    lp = os.path.join(leg_dir, name)
    if not os.path.exists(lp):
        continue
    E, ev = load(os.path.join(enh_dir, name)); L, lv = load(lp)
    if E is None or L is None or ev == lv:
        continue
    ge, gl = shader_groups(E, False), shader_groups(L, True)
    if len(ge) != len(gl):
        continue
    files += 1
    for se_list, sl_list in zip(ge, gl):
        if len(se_list) != len(sl_list):
            continue
        for se, sl in zip(se_list, sl_list):
            effect, legacy = legacy_textures(L, sl)
            effect2, enhanced = enhanced_textures(E, se)
            if effect != effect2:
                continue
            enames = collections.Counter(n.lower() for h, n in enhanced if n)
            lnames = collections.Counter(n.lower() for h, n in legacy if n)
            by_name = {n.lower(): h for h, n in enhanced if n and enames[n.lower()] == 1}
            for h, n in legacy:
                if n and lnames[n.lower()] == 1 and n.lower() in by_name:
                    pairs[(effect, h)][by_name[n.lower()]] += 1
                    anyeffect[h][by_name[n.lower()]] += 1
print(f"{files} files paired, {len(pairs)} (effect, name) pairs, {len(anyeffect)} legacy names", file=sys.stderr)
conflicts = {h: c for h, c in anyeffect.items() if len(c) > 1}
print(f"{len(conflicts)} legacy names map to more than one Enhanced name", file=sys.stderr)
for h, c in list(conflicts.items())[:10]:
    print(f"  {h:08x}: {dict((f'{k:08x}', v) for k, v in c.items())}", file=sys.stderr)
json.dump({"global": {f"{h:08x}": f"{c.most_common(1)[0][0]:08x}" for h, c in anyeffect.items()},
           "per_effect": {f"{e:08x}:{h:08x}": f"{c.most_common(1)[0][0]:08x}" for (e, h), c in pairs.items()}},
          open(sys.argv[3], "w"), indent=0, sort_keys=True)
