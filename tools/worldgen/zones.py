"""Population zones for the world builder's lands (research/phase0.md §62): the land's place in the game's world cut into
1 km blocks (block (i, j): x from 1000 i, y from 1000 j), each bound like a game zone of its kind - B beach, U town
(city or suburbs), R country - and named after the nearest place the map has. All the lands share the blocks: one mod holds every
block's three zones (`--mod`: popzone.ipl lines and their game zones for worldgen zones, written once - the game holds
about 1900 zones, its own 1322 among them); the loader (game/sectors) enables the one of each block's three that the land
there has and shows the block's name for it.

usage: python -I tools/worldgen/zones.py --world <world.json> --land <name>_land.png [--landcover <landcover.bin>]
       --out <land.json>     the land's blocks into its land.json ("zones": [[i, j, kind, name], ...])
       python -I tools/worldgen/zones.py --mod <zones.txt>
"""
import json, math, struct, sys

import numpy as np
from PIL import Image

I0, I1, J0, J1 = 4, 16, -3, 10  # (the blocks over the lands' box: x 4000..17000, y -3000..11000)
LIKE = {'B': 'VesBe3', 'U': 'Z_DELPa', 'R': 'Grape'}
# the land picture's classes (worldgen build's <name>_land.png)
COLOURS = {(40, 90, 160): 'water', (150, 190, 110): 'land', (235, 220, 160): 'beach', (180, 170, 160): 'urban', (90, 150, 70): 'park',
           (120, 120, 120): 'paved'}
# how far a place's name reaches (m): a city's further than a hamlet's
REACH = {'city': 7000, 'town': 4000, 'village': 2500, 'suburb': 1800, 'quarter': 1200, 'neighbourhood': 900, 'hamlet': 600, 'locality': 500,
         'island': 1500}


def options(argv):
    o = {}
    for i in range(0, len(argv) - 1, 2):
        if not argv[i].startswith('--'):
            sys.exit(__doc__)
        o[argv[i][2:]] = argv[i + 1]
    return o


def Name(i, j):
    """A block's zones' name and label: W, then i and j + 3 in two digits each (the game keeps 7 characters of a zone's
    name: WB10_-3U had been cut off)."""
    return f'W{i:02d}{j - J0:02d}'


def mod(out):
    with open(out, 'w', encoding='utf-8') as f:
        for j in range(J0, J1 + 1):
            for i in range(I0, I1 + 1):
                for kind, like in LIKE.items():
                    f.write(f'{Name(i, j)}{kind}, {i * 1000}.0, {j * 1000}.0, -100.0, {(i + 1) * 1000}.0, {(j + 1) * 1000}.0, 1250.0, {Name(i, j)}, {like}\n')
    print(f'{(I1 - I0 + 1) * (J1 - J0 + 1) * len(LIKE)} zones in {out}')


def main():
    o = options(sys.argv[1:])
    if 'mod' in o:
        return mod(o['mod'])
    if not all(k in o for k in ('world', 'land', 'out')):
        sys.exit(__doc__)
    w = json.load(open(o['world'], encoding='utf-8'))
    # the land picture: 4 m cells over the world's box, its top row the north
    pic = np.array(Image.open(o['land']).convert('RGB'))
    h, wd = pic.shape[:2]
    cls = np.zeros((h, wd), np.uint8)  # 0 water, 1 land, 2 beach, 3 urban/paved
    for colour, name in COLOURS.items():
        m = np.all(pic == np.array(colour, np.uint8), axis=-1)
        cls[m] = {'water': 0, 'land': 1, 'park': 1, 'beach': 2, 'urban': 3, 'paved': 3}[name]
    cls = cls[::-1]  # (rows south to north)
    x0, y0 = w['min'][0], w['min'][1]
    cell = (w['max'][0] - x0) / wd
    cover = None
    if 'landcover' in o:
        b = open(o['landcover'], 'rb').read()
        _, lx0, ly0, lc, lnx, lny = struct.unpack('<I3f2I', b[4:28])
        cover = (lx0, ly0, lc, np.frombuffer(b[28:], np.uint8).reshape(lny, lnx))
    # (not a trailer park's or a mobile home park's name: hamlets the map has many of)
    places = [(p['at'][0], p['at'][1], p['kind'][6:], p['name']) for p in w.get('points', []) if p['kind'].startswith('place:') and p.get('name')
              and not any(word in p['name'].lower() for word in ('mobile home', 'trailer', 'rv park', 'rv resort', 'campground', 'estates mobile'))]
    zones = []
    for j in range(J0, J1 + 1):
        for i in range(I0, I1 + 1):
            bx0, by0 = i * 1000, j * 1000
            c0, c1 = int((bx0 - x0) / cell), int((bx0 + 1000 - x0) / cell)
            r0, r1 = int((by0 - y0) / cell), int((by0 + 1000 - y0) / cell)
            c0, c1, r0, r1 = max(c0, 0), min(c1, wd), max(r0, 0), min(r1, h)
            if c1 <= c0 or r1 <= r0:
                continue
            block = cls[r0:r1, c0:c1]
            land = int(np.count_nonzero(block))
            if land < 0.05 * block.size:
                continue
            beach = int(np.count_nonzero(block == 2))
            kind = 'B' if beach > 0.15 * land else None
            if kind is None and cover is not None:
                lx0, ly0, lc, lcv = cover
                a0, a1 = max(int((bx0 - lx0) / lc), 0), min(int((bx0 + 1000 - lx0) / lc), lcv.shape[1])
                b0, b1 = max(int((by0 - ly0) / lc), 0), min(int((by0 + 1000 - ly0) / lc), lcv.shape[0])
                sub = lcv[b0:b1, a0:a1]
                known = max(1, int(np.count_nonzero((sub != 0) & (sub != 11))))
                high = np.count_nonzero((sub == 23) | (sub == 24)) / known
                low = np.count_nonzero((sub == 21) | (sub == 22)) / known
                kind = 'U' if high + low > 0.35 else 'R'
            if kind is None:
                kind = 'U'
            # the name: the place nearest by its reach (its kind's), within 1.5 of it, else the nearest town or city
            cx, cy = bx0 + 500, by0 + 500
            best, name = 1.5, ''
            for px, py, k, n in places:
                score = math.hypot(px - cx, py - cy) / REACH.get(k, 1000)
                if score < best:
                    best, name = score, n
            if not name:
                towns = [(math.hypot(px - cx, py - cy), n) for px, py, k, n in places if k in ('city', 'town', 'village')]
                if towns:
                    name = min(towns)[1]
            zones.append([i, j, kind, name])
    land = json.load(open(o['out'], encoding='utf-8'))
    land['zones'] = zones
    with open(o['out'], 'w', encoding='utf-8') as f:
        json.dump(land, f, ensure_ascii=False)
    kinds = {k: sum(1 for z in zones if z[2] == k) for k in LIKE}
    print(f'zones: {len(zones)} blocks ({", ".join(f"{k} {n}" for k, n in kinds.items())}), {len(places)} places, '
          f'{len(set(z[3] for z in zones if z[3]))} names')


if __name__ == '__main__':
    main()
