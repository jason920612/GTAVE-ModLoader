"""Dump the parameter layout of every shader effect loaded in the running game (research aid / data for convert_yft.py).
usage: dump_effects.py <out.json>

Effects live in a hash map (GTA5_Enhanced.exe +0x4F05C18: list head; nodes {next, prev, u32 name hash @0x10,
effect @0x18}). effect -> +0x00 data; data +0x38 parameter descriptors, 12 bytes each:
  u16 index, u8 type (0 texture, 1 buffer, 2 sampler, 3 constant), u8 register, u32 old name hash, u32 name hash.
data +0x50 = bytes of the per-instance parameter block, data +0x5C = descriptor count. "Old name" is the
variable name legacy files use for constants; the game matches a resource's parameter table against "name"
(research/phase0.md §16).
Output: {effect hash: {"textures": [name hash in index order], "constants": {old hash: name hash}, "block": bytes}}
"""
import json, struct, sys
import pymem

EFFECT_LIST = 0x4F05C18

def main():
    pm = pymem.Pymem("GTA5_Enhanced.exe")
    q = pm.read_ulonglong
    head = q(pm.process_base.lpBaseOfDll + EFFECT_LIST)
    out, node = {}, q(head)
    while node != head:
        name, data = pm.read_uint(node + 0x10), q(q(node + 0x18))
        count = pm.read_uchar(data + 0x5C)
        raw = pm.read_bytes(q(data + 0x38), 12 * count)
        textures, constants = [], {}
        for i in range(count):
            index, kind = struct.unpack_from("<HB", raw, 12 * i)
            old, new = struct.unpack_from("<II", raw, 12 * i + 4)
            if kind == 0:
                textures.append((index, f"{new:08x}"))
            elif kind == 3:
                constants[f"{old:08x}"] = f"{new:08x}"
        out[f"{name:08x}"] = {"textures": [h for _, h in sorted(textures)], "constants": constants,
                              "block": pm.read_uint(data + 0x50)}
        node = q(node)
    json.dump(out, open(sys.argv[1], "w"), indent=0, sort_keys=True)
    print(f"{len(out)} effects -> {sys.argv[1]}")

if __name__ == "__main__":
    main()
