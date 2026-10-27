"""Elevation from airborne laser scans (research/phase0.md §61.18) for the world builder: public point clouds (USGS
3DEP, NOAA Digital Coast) -> rasters in game coordinates that worldgen reads: the top surface (roofs, tree crowns), the
ground, water, bridge decks, buildings and trees as the scan's own classes say.

usage (in a Python with laspy[lazrs], numpy and pyproj; keep the data outside the repository):
  lidar.py fetch  --index <url> --world <world.json> [--area x0,y0,x1,y1] --out <laz dir> [--crs <epsg>]
  lidar.py ept    --ept <url> --world <world.json> [--area x0,y0,x1,y1] --out <laz dir>
  lidar.py raster --world <world.json> --laz <laz dir> --out <raster dir> [--area x0,y0,x1,y1] [--cell 0.5] [--tile 256]
                  [--crs <epsg>]

fetch: the scan's files that touch the world's box (or the area of it, in game metres), downloaded once (resumable).
The index is either a USGS project's virtual point cloud (.vpc: a STAC FeatureCollection, each file's box in longitude
and latitude, its asset relative to the .vpc) or a NOAA project's minmax .csv (each file's box in the files' own
coordinates, --crs, default 6346: NAD83(2011) UTM 17N; the files next to the .csv).
ept: the same from an Entwine point tile set - USGS's public bucket (s3-us-west-2.amazonaws.com/usgs-lidar-public/<project>:
its 3DEP projects, no rate limit as on rockyweb) -: the octree's nodes over the area, each node's file as it is (EPSG:3857,
no coordinate system in their headers: raster --crs 3857).
raster: every file's points into the world's raster tiles, `t_<i>_<j>.bin` (i = floor(x / tile), j = floor(y / tile)):
  header (32 bytes): 'WBLR', u32 version 2, f32 x0, f32 y0 (the tile's south-west corner), f32 cell, u32 width,
  u32 height, u32 layers (5); then the layers, each width * height, rows south to north:
    top    i16  the highest return (cm above NAVD88; -32768: none)  - roofs, crowns, the ground where it is bare
    ground i16  the mean of the ground returns (class 2)            - holes under buildings and trees
    roof   i16  the highest building return (class 6)               - roofs without the trees over them
    count  u8   returns in the cell (saturating)
    flags  u8   1 water (class 9), 2 bridge deck (17), 4 building (6), 8 vegetation (3-5)
Points are turned from each file's own coordinates (its header's WKT, read with pyproj) to latitude and longitude
(NAD83(2011); the world's OpenStreetMap footprints in Miami came from the county's own NAD83 data) and then by the
world's projection (world.json "projection"). Partial tiles are kept between files (<out>/partial) so that tiles on
the files' seams get the points of every file; `raster` keeps a list of the files done and can be run again.
"""
import csv, json, os, shutil, struct, sys, time, urllib.request

def options(argv):
    o = {}
    i = 0
    while i < len(argv):
        if argv[i].startswith('--') and i + 1 < len(argv):
            o[argv[i][2:]] = argv[i + 1]
            i += 2
        else:
            i += 1
    return o

def load_world(path):
    with open(path, encoding='utf-8') as f:
        w = json.load(f)
    p = w.get('projection')
    if not p:
        sys.exit(path + ' has no "projection" (import it again with worldgen osm)')
    return w, p

def area_of(o, w):
    if 'area' in o:
        x0, y0, x1, y1 = (float(v) for v in o['area'].split(','))
        return x0, y0, x1, y1
    return w['min'][0], w['min'][1], w['max'][0], w['max'][1]

def to_latlon(p, x, y):
    return p['lat0'] + (y - p['y0']) / p['ky'], p['lon0'] + (x - p['x0']) / p['kx']

AGENT = {'User-Agent': 'GTAVE-ModLoader-worldgen/1.0 (laser scans for a game map of Florida)'}


def download(url, file):
    # (resumed where a try stopped; USGS's server answers 429 to a quick run of requests: waited out, longer each time)
    for attempt in range(9):
        try:
            have = os.path.getsize(file + '.part') if os.path.exists(file + '.part') else 0
            headers = dict(AGENT, **({'Range': f'bytes={have}-'} if have else {}))
            with urllib.request.urlopen(urllib.request.Request(url, headers=headers), timeout=600) as r:
                with open(file + '.part', 'ab' if have and r.status == 206 else 'wb') as f:
                    shutil.copyfileobj(r, f, 1 << 20)
            os.replace(file + '.part', file)
            return
        except OSError as e:
            wait = 30 * 2 ** min(attempt, 4)
            print(f'  {os.path.basename(file)}: {e} (try {attempt + 1}; again in {wait} s)', flush=True)
            time.sleep(wait)
    sys.exit(f'cannot download {url}')

def fetch(o):
    w, p = load_world(o['world'])
    x0, y0, x1, y1 = area_of(o, w)
    s, west = to_latlon(p, x0, y0)
    n, east = to_latlon(p, x1, y1)
    out = o['out']
    os.makedirs(out, exist_ok=True)
    url = o['index']
    base = url.rsplit('/', 1)[0] + '/'
    index = os.path.join(out, url.rsplit('/', 1)[1])
    if not os.path.exists(index):
        download(url, index)
    wanted = [] # (file name, url, bytes or 0)
    if index.endswith('.vpc'):
        with open(index, encoding='utf-8') as f:
            vpc = json.load(f)
        for t in vpc['features']:
            b = t['bbox'] # lon, lat, z, lon, lat, z
            if b[0] < east and b[3] > west and b[1] < n and b[4] > s:
                href = next(iter(t['assets'].values()))['href']
                href = href[2:] if href.startswith('./') else href
                wanted.append((href.rsplit('/', 1)[1], href if href.startswith('http') else base + href))
    else:
        import pyproj
        tr = pyproj.Transformer.from_crs(6318, int(o.get('crs', 6346)), always_xy=True)
        xs, ys = tr.transform([west, east, west, east], [s, s, n, n])
        bx0, bx1, by0, by1 = min(xs), max(xs), min(ys), max(ys)
        with open(index, newline='') as f:
            rows = csv.reader(f)
            next(rows)
            for r in rows:
                # (the minmax files write the last values as a tuple of numpy floats: keep the numbers alone)
                v = [float(''.join(ch for ch in c if ch in '0123456789.-e')) for c in ','.join(r[1:]).replace('np.float64', '').split(',') if c.strip(' ()')]
                if v[0] < bx1 and v[1] > bx0 and v[2] < by1 and v[3] > by0:
                    wanted.append((r[0].strip(), base + r[0].strip()))
    print(f'{len(wanted)} files', flush=True)
    start = time.time()
    total = 0
    for k, (name, href) in enumerate(wanted):
        file = os.path.join(out, name)
        if os.path.exists(file):
            continue
        download(href, file)
        total += os.path.getsize(file)
        if (k + 1) % 10 == 0 or k + 1 == len(wanted):
            print(f'[{k + 1}/{len(wanted)}] {total / 1e6:.0f} MB, {total / 1e6 / max(1, time.time() - start):.1f} MB/s', flush=True)

def get_json(url):
    for attempt in range(6):
        try:
            with urllib.request.urlopen(urllib.request.Request(url, headers=AGENT), timeout=120) as r:
                return json.load(r)
        except OSError as e:
            print(f'  {url}: {e} (try {attempt + 1})', flush=True)
            time.sleep(10 * (attempt + 1))
    sys.exit(f'cannot read {url}')


def ept(o):
    import math
    w, p = load_world(o['world'])
    x0, y0, x1, y1 = area_of(o, w)
    s, west = to_latlon(p, x0, y0)
    n, east = to_latlon(p, x1, y1)
    # (the area in web Mercator, the set's coordinates)
    R = 6378137.0
    mx0, mx1 = R * math.radians(west), R * math.radians(east)
    my0, my1 = (R * math.log(math.tan(math.pi / 4 + math.radians(v) / 2)) for v in (s, n))
    base = o['ept'].rstrip('/')
    out = o['out']
    os.makedirs(out, exist_ok=True)
    b = get_json(base + '/ept.json')['bounds']  # min x, y, z, max x, y, z: a cube
    wanted, pending, points = [], ['0-0-0-0'], 0
    while pending:
        root = pending.pop()
        for key, count in get_json(f'{base}/ept-hierarchy/{root}.json').items():
            d, i, j, _ = (int(v) for v in key.split('-'))
            size = (b[3] - b[0]) / 2 ** d
            nx0, ny0 = b[0] + i * size, b[1] + j * size
            if nx0 > mx1 or nx0 + size < mx0 or ny0 > my1 or ny0 + size < my0:
                continue
            if count == -1:
                pending.append(key)  # (its own hierarchy file)
            elif count > 0:
                wanted.append(key)
                points += count
    print(f'{len(wanted)} nodes, {points / 1e6:.0f} million points', flush=True)
    # (16 at a time: each node's file is small, the bucket far away - one after another ran at 0.3 MB/s)
    import concurrent.futures
    start, total, done = time.time(), 0, 0

    def one(key):
        file = os.path.join(out, key + '.laz')
        if os.path.exists(file):
            return 0
        download(f'{base}/ept-data/{key}.laz', file)
        return os.path.getsize(file)

    with concurrent.futures.ThreadPoolExecutor(16) as pool:
        for size in pool.map(one, wanted):
            total += size
            done += 1
            if done % 500 == 0 or done == len(wanted):
                print(f'[{done}/{len(wanted)}] {total / 1e6:.0f} MB, {total / 1e6 / max(1, time.time() - start):.1f} MB/s', flush=True)


class Partial:
    """A raster tile while points come in: the highest return, the ground's sum and count, returns, flags."""
    def __init__(self, np, n):
        self.top = np.full(n * n, -1e9, np.float32)
        self.roof = np.full(n * n, -1e9, np.float32)
        self.gsum = np.zeros(n * n, np.float64)
        self.gcount = np.zeros(n * n, np.uint32)
        self.count = np.zeros(n * n, np.uint32)
        self.flags = np.zeros(n * n, np.uint8)

def raster(o):
    import numpy as np
    import laspy, pyproj
    w, p = load_world(o['world'])
    ax0, ay0, ax1, ay1 = area_of(o, w)
    cell = float(o.get('cell', 0.5))
    size = float(o.get('tile', 256))
    n = int(round(size / cell))
    out = o['out']
    partial = os.path.join(out, 'partial')
    os.makedirs(partial, exist_ok=True)
    done_list = os.path.join(out, 'done.txt')
    done = set(open(done_list).read().split()) if os.path.exists(done_list) else set()
    laz = o['laz']
    files = sorted(f for f in os.listdir(laz) if f.endswith('.laz') and f not in done)
    transformers = {}
    start_all = time.time()
    for k, name in enumerate(files):
        start = time.time()
        tiles = {}
        points = 0
        with laspy.open(os.path.join(laz, name)) as r:
            crs = r.header.parse_crs()
            if crs is None and 'crs' in o:
                crs = pyproj.CRS.from_epsg(int(o['crs']))
            if crs is None:
                print(f'{name}: no coordinate system in its header, skipped')
                continue
            horizontal = crs.sub_crs_list[0] if crs.is_compound else crs
            key = horizontal.to_wkt()
            if key not in transformers:
                transformers[key] = pyproj.Transformer.from_crs(horizontal, horizontal.geodetic_crs, always_xy=True)
            tr = transformers[key]
            zunit = (crs.sub_crs_list[1] if crs.is_compound else horizontal).axis_info[0].unit_conversion_factor
            # the file's box first: files outside the area are not read
            mins, maxs = r.header.mins, r.header.maxs
            lon, lat = tr.transform([mins[0], maxs[0], mins[0], maxs[0]], [mins[1], mins[1], maxs[1], maxs[1]])
            fx = [p['x0'] + (v - p['lon0']) * p['kx'] for v in lon]
            fy = [p['y0'] + (v - p['lat0']) * p['ky'] for v in lat]
            if min(fx) > ax1 or max(fx) < ax0 or min(fy) > ay1 or max(fy) < ay0:
                continue
            for chunk in r.chunk_iterator(4_000_000):
                cls = np.asarray(chunk.classification)
                keep = (cls != 7) & (cls != 18)
                if 'withheld' in chunk.point_format.dimension_names:
                    keep &= ~np.asarray(chunk.withheld, dtype=bool)
                lon, lat = tr.transform(np.asarray(chunk.x)[keep], np.asarray(chunk.y)[keep])
                x = p['x0'] + (lon - p['lon0']) * p['kx']
                y = p['y0'] + (lat - p['lat0']) * p['ky']
                z = (np.asarray(chunk.z)[keep] * zunit).astype(np.float32)
                c = cls[keep]
                inside = (x >= ax0) & (x < ax1) & (y >= ay0) & (y < ay1)
                x, y, z, c = x[inside], y[inside], z[inside], c[inside]
                points += len(z)
                ti = np.floor(x / size).astype(np.int64)
                tj = np.floor(y / size).astype(np.int64)
                ci = np.clip(((x - ti * size) / cell).astype(np.int64), 0, n - 1)
                cj = np.clip(((y - tj * size) / cell).astype(np.int64), 0, n - 1)
                key = (ti + 50000) * 100000 + (tj + 50000)
                order = np.argsort(key, kind='stable')
                key, ci, cj, z, c = key[order], ci[order], cj[order], z[order], c[order]
                bounds = np.flatnonzero(np.diff(key)) + 1
                for a, b in zip(np.concatenate(([0], bounds)), np.concatenate((bounds, [len(key)]))):
                    if a == b:
                        continue
                    kk = int(key[a])
                    t = tiles.get(kk)
                    if t is None:
                        t = tiles[kk] = Partial(np, n)
                    at = cj[a:b] * n + ci[a:b]
                    np.maximum.at(t.top, at, z[a:b])
                    np.add.at(t.count, at, 1)
                    g = c[a:b] == 6
                    if g.any():
                        np.maximum.at(t.roof, at[g], z[a:b][g])
                    g = c[a:b] == 2
                    np.add.at(t.gsum, at[g], z[a:b][g])
                    np.add.at(t.gcount, at[g], 1)
                    for cls_value, bit in ((9, 1), (17, 2), (6, 4), (3, 8), (4, 8), (5, 8)):
                        m = c[a:b] == cls_value
                        if m.any():
                            t.flags[at[m]] |= bit
        # merge into the partial tiles on disk
        for kk, t in tiles.items():
            ti, tj = kk // 100000 - 50000, kk % 100000 - 50000
            file = os.path.join(partial, f'p_{ti}_{tj}.npz')
            if os.path.exists(file):
                with np.load(file) as old: # (closed before the file is replaced)
                    t.top = np.maximum(t.top, old['top'])
                    t.roof = np.maximum(t.roof, old['roof'])
                    t.gsum += old['gsum']
                    t.gcount += old['gcount']
                    t.count += old['count']
                    t.flags |= old['flags']
            np.savez(file + '.tmp.npz', top=t.top, roof=t.roof, gsum=t.gsum, gcount=t.gcount, count=t.count, flags=t.flags)
            os.replace(file + '.tmp.npz', file)
        done.add(name)
        with open(done_list, 'a') as f:
            f.write(name + '\n')
        if points:
            print(f'[{k + 1}/{len(files)}] {name}: {points / 1e6:.1f} million points, {len(tiles)} tiles, {time.time() - start:.0f} s', flush=True)
    print(f'points read in {time.time() - start_all:.0f} s', flush=True)
    # the finished tiles
    count = 0
    for file in sorted(os.listdir(partial)):
        if not file.startswith('p_') or not file.endswith('.npz'):
            continue
        ti, tj = (int(v) for v in file[2:-4].split('_'))
        with np.load(os.path.join(partial, file)) as f:
            t = {k: f[k] for k in f.files}
        top = np.where(t['top'] > -1e8, np.round(t['top'] * 100), -32768).clip(-32768, 32767).astype('<i2')
        roof = np.where(t['roof'] > -1e8, np.round(t['roof'] * 100), -32768).clip(-32768, 32767).astype('<i2')
        g = t['gcount'] > 0
        ground = np.full(n * n, -32768, np.float64)
        ground[g] = np.round(t['gsum'][g] / t['gcount'][g] * 100)
        ground = ground.clip(-32768, 32767).astype('<i2')
        cnt = np.minimum(t['count'], 255).astype(np.uint8)
        with open(os.path.join(out, f't_{ti}_{tj}.bin'), 'wb') as f:
            f.write(b'WBLR' + struct.pack('<Ifff3I', 2, ti * size, tj * size, cell, n, n, 5))
            f.write(top.tobytes())
            f.write(ground.tobytes())
            f.write(roof.tobytes())
            f.write(cnt.tobytes())
            f.write(t['flags'].astype(np.uint8).tobytes())
        count += 1
    print(f'{count} raster tiles in {out}')

if __name__ == '__main__':
    if len(sys.argv) < 2 or sys.argv[1] not in ('fetch', 'ept', 'raster'):
        sys.exit(__doc__)
    o = options(sys.argv[2:])
    {'fetch': fetch, 'ept': ept, 'raster': raster}[sys.argv[1]](o)
