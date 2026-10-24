"""The pixel shaders the game draws its decals with (bullet holes, blood, scorch marks: its decal manager's), by the digest
every compiled shader carries, for the loader to know the pipelines and the command lists that put them on the world
(src/loader/graphics/particles.cpp, research/phase0.md §60.17). Only the deferred ones, which draw into the G-buffer
(PS_Vfx_Decal_DeferredTextured*, PS_decalFxDeferred*): their forward-lit kinds draw into the scene colour, later.
usage: decal_shaders.py <sga_win32_60_final.awc> <out .inc>   (programs as tools/ptfx_shaders.py reads them)"""
import os, sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from ptfx_shaders import programs  # noqa: E402

PREFIXES = ("PS_Vfx_Decal_Deferred", "PS_decalFxDeferred")


def main():
    data = open(sys.argv[1], "rb").read()
    if data[:4] != b"SGD2":
        raise SystemExit("not an SGD2 container")
    decal = sorted({(d, n) for n, d, t in programs(data) if n.startswith(PREFIXES)})
    with open(sys.argv[2], "w", newline="\n") as f:
        f.write(f"// {len(decal)} decal pixel shaders (tools/decal_shaders.py): {{low, high}} of the digest, little-endian\n")
        for d, n in decal:
            low = int.from_bytes(d[:8], "little")
            high = int.from_bytes(d[8:16], "little")
            f.write(f"{{0x{low:016x}ull, 0x{high:016x}ull}}, // {n}\n")
    print(f"{len(decal)} decal pixel shaders")


if __name__ == "__main__":
    main()
