"""Building footprints where the map has none (research/phase0.md §63): Microsoft's US Building Footprints (ODbL, as
OpenStreetMap; footprints traced from aerial pictures by machine, no heights), added to a sector's world description
where no OpenStreetMap building stands - whole streets of houses the map lacks.

usage: python -I tools/worldgen/msbuildings.py split --zip <Florida.geojson.zip> --out <cells dir>
       python -I tools/worldgen/msbuildings.py merge --world <world.json> --cells <cells dir> [--cell c,r]
  split: each footprint (its outer ring) into the sectors' grid (tools/worldgen/sectors.py: 0.121 x 0.117 degrees from
         25.745 N, -80.232 E) by its middle: <cells dir>/<column>_<row>.jsonl, a ring of [lon, lat] a line
  merge: the cell's footprints (by the world's box, its neighbours' too where they reach in) in game coordinates, those
         that overlap none of the world's buildings, as buildings of kind "yes" (a house's height under 350 m², else 7 m);
         world.json written again (its "attribution" told)
"""
import io, json, math, os, sys, zipfile

LAT0, LON0, DLAT, DLON = 25.745, -80.232, 0.121, 0.117


def options(argv):
    o = {}
    for i in range(0, len(argv) - 1, 2):
        if not argv[i].startswith('--'):
            sys.exit(__doc__)
        o[argv[i][2:]] = argv[i + 1]
    return o


def split(o):
    out = o['out']
    os.makedirs(out, exist_ok=True)
    files = {}
    count = 0
    with zipfile.ZipFile(o['zip']) as z:
        name = next(n for n in z.namelist() if n.endswith('.geojson'))
        with z.open(name) as raw:
            for line in io.TextIOWrapper(raw, encoding='utf-8'):
                line = line.strip().rstrip(',')
                if not line.startswith('{"type":"Feature"') and not line.startswith('{ "type": "Feature"'):
                    continue
                try:
                    f = json.loads(line)
                except ValueError:
                    continue
                g = f.get('geometry') or {}
                if g.get('type') == 'Polygon':
                    ring = g['coordinates'][0]
                elif g.get('type') == 'MultiPolygon':
                    ring = g['coordinates'][0][0]
                else:
                    continue
                lon = sum(p[0] for p in ring) / len(ring)
                lat = sum(p[1] for p in ring) / len(ring)
                c, r = math.floor((lon - LON0) / DLON), math.floor((lat - LAT0) / DLAT)
                key = (c, r)
                if key not in files:
                    if len(files) > 400:  # (not too many open at once)
                        for fh in files.values():
                            fh.close()
                        files = {}
                    files[key] = open(os.path.join(out, f'{c}_{r}.jsonl'), 'a', encoding='utf-8')
                files[key].write(json.dumps([[round(p[0], 7), round(p[1], 7)] for p in ring]) + '\n')
                count += 1
                if count % 500000 == 0:
                    print(f'{count} footprints', flush=True)
    for fh in files.values():
        fh.close()
    print(f'{count} footprints split')


def inside(ring, x, y):
    hit = False
    j = len(ring) - 1
    for i in range(len(ring)):
        xi, yi = ring[i]
        xj, yj = ring[j]
        if (yi > y) != (yj > y) and x < (xj - xi) * (y - yi) / (yj - yi) + xi:
            hit = not hit
        j = i
    return hit


def area(ring):
    return abs(sum(ring[i][0] * ring[i - 1][1] - ring[i - 1][0] * ring[i][1] for i in range(len(ring)))) / 2


def merge(o):
    with open(o['world'], encoding='utf-8') as f:
        w = json.load(f)
    p = w['projection']
    x0, y0, x1, y1 = w['min'][0], w['min'][1], w['max'][0], w['max'][1]
    # (as worldgen's projection: the place in its plane on a lattice of 1/1024 m, then the land's origin - each coordinate a
    # float exactly, the same in every land less its origin)
    snap = lambda v: round(v * 1024) / 1024
    to_game = lambda lon, lat: (p['x0'] + snap((lon - p['lon0']) * p['kx']), p['y0'] + snap((lat - p['lat0']) * p['ky']))
    to_ll = lambda x, y: (p['lon0'] + (x - p['x0']) / p['kx'], p['lat0'] + (y - p['y0']) / p['ky'])
    # the cells the box reaches into
    lon0, lat0 = to_ll(x0, y0)
    lon1, lat1 = to_ll(x1, y1)
    cells = [(c, r) for c in range(math.floor((lon0 - LON0) / DLON), math.floor((lon1 - LON0) / DLON) + 1)
             for r in range(math.floor((lat0 - LAT0) / DLAT), math.floor((lat1 - LAT0) / DLAT) + 1)]
    # the world's footprints, by 50 m cell
    grid = {}
    for b in w['buildings']:
        ring = b['footprint']
        xs, ys = [q[0] for q in ring], [q[1] for q in ring]
        for gi in range(int(min(xs) // 50), int(max(xs) // 50) + 1):
            for gj in range(int(min(ys) // 50), int(max(ys) // 50) + 1):
                grid.setdefault((gi, gj), []).append(ring)
    added = 0
    for c, r in cells:
        path = os.path.join(o['cells'], f'{c}_{r}.jsonl')
        if not os.path.exists(path):
            continue
        with open(path, encoding='utf-8') as f:
            for line in f:
                ring = [list(to_game(q[0], q[1])) for q in json.loads(line)]
                if len(ring) > 3 and ring[0] == ring[-1]:
                    ring.pop()
                if len(ring) < 3:
                    continue
                cx, cy = sum(q[0] for q in ring) / len(ring), sum(q[1] for q in ring) / len(ring)
                if not (x0 <= cx < x1 and y0 <= cy < y1):
                    continue
                # (over one of the map's footprints: its middle in one, or one's middle in it)
                near = grid.get((int(cx // 50), int(cy // 50)), [])
                if any(inside(other, cx, cy) or inside(ring, sum(q[0] for q in other) / len(other), sum(q[1] for q in other) / len(other))
                       for other in near):
                    continue
                a = area(ring)
                if a < 25:
                    continue
                # (anticlockwise, as the world's footprints are)
                if sum(ring[i][0] * ring[i - 1][1] - ring[i - 1][0] * ring[i][1] for i in range(len(ring))) > 0:
                    ring.reverse()
                w['buildings'].append({'kind': 'yes', 'name': '', 'height': 4.5 if a < 350 else 7.0, 'footprint': [[q[0], q[1]] for q in ring]})
                added += 1
    note = 'Building footprints: Microsoft US Building Footprints (ODbL)'
    if note not in w.get('attribution', ''):
        w['attribution'] = (w.get('attribution', '') + '; ' + note).strip('; ')
    with open(o['world'], 'w', encoding='utf-8') as f:
        json.dump(w, f, ensure_ascii=False)
    print(f'{added} footprints the map lacked added ({len(w["buildings"])} buildings now)')


def main():
    if len(sys.argv) < 2 or sys.argv[1] not in ('split', 'merge'):
        sys.exit(__doc__)
    o = options(sys.argv[2:])
    if sys.argv[1] == 'split':
        if 'zip' not in o or 'out' not in o:
            sys.exit(__doc__)
        split(o)
    else:
        if 'world' not in o or 'cells' not in o:
            sys.exit(__doc__)
        merge(o)


if __name__ == '__main__':
    main()
