"""Aerial pictures (research/phase0.md §61.18) for the world builder: the USDA's NAIP orthophotos (public domain; 0.3 m
a pixel in Florida from 2023: red, green, blue, near infrared), through Microsoft's Planetary Computer, resampled into
tiles in game coordinates that worldgen reads (roofs, the ground's cover).

usage (in a Python with numpy, pyproj, tifffile and zarr; keep the data outside the repository):
  aerial.py fetch  --world <world.json> [--area x0,y0,x1,y1] --out <image dir> [--year 2023]
  aerial.py raster --world <world.json> --images <image dir> --out <tile dir> [--area x0,y0,x1,y1] [--cell 0.25] [--tile 256]

fetch: the year's pictures that touch the area (its STAC search), downloaded once (the files are cloud-optimized
GeoTIFFs; items.json keeps each one's coordinate system and transform).
raster: tiles `a_<i>_<j>.bin` (i = floor(x / tile), j = floor(y / tile)): header (32 bytes): 'WBAI', u32 version 1,
f32 x0, f32 y0 (the tile's south-west corner), f32 cell, u32 width, u32 height, u32 bands (4); then the pixels, rows
south to north, each red, green, blue, near infrared (u8; all 0: no picture). A pixel's centre is turned from game
metres to latitude and longitude (the world's projection) and then to the picture's coordinates (pyproj), and read
bilinearly.
"""
import json, os, struct, sys, time, urllib.request

STAC = 'https://planetarycomputer.microsoft.com/api/stac/v1/search'
TOKEN = 'https://planetarycomputer.microsoft.com/api/sas/v1/token/naip'

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
    if 'projection' not in w:
        sys.exit(path + ' has no "projection" (import it again with worldgen osm)')
    return w, w['projection']

def area_of(o, w):
    if 'area' in o:
        return tuple(float(v) for v in o['area'].split(','))
    return w['min'][0], w['min'][1], w['max'][0], w['max'][1]

def to_latlon(p, x, y):
    return p['lat0'] + (y - p['y0']) / p['ky'], p['lon0'] + (x - p['x0']) / p['kx']

def fetch(o):
    w, p = load_world(o['world'])
    x0, y0, x1, y1 = area_of(o, w)
    s, west = to_latlon(p, x0, y0)
    n, east = to_latlon(p, x1, y1)
    out = o['out']
    os.makedirs(out, exist_ok=True)
    body = json.dumps({'collections': ['naip'], 'bbox': [west, s, east, n], 'limit': 200}).encode()
    req = urllib.request.Request(STAC, body, {'Content-Type': 'application/json'})
    items = json.load(urllib.request.urlopen(req))['features']
    year = o.get('year') or max(i['properties']['naip:year'] for i in items)
    items = [i for i in items if i['properties']['naip:year'] == year]
    print(f'{len(items)} pictures of {year}', flush=True)
    listing = os.path.join(out, 'items.json')
    known = json.load(open(listing)) if os.path.exists(listing) else {}
    for k, it in enumerate(items):
        name = it['id'] + '.tif'
        known[name] = {'epsg': it['properties']['proj:epsg'], 'transform': it['properties']['proj:transform'],
                       'shape': it['properties']['proj:shape'], 'year': year, 'gsd': it['properties']['gsd']}
        file = os.path.join(out, name)
        if not os.path.exists(file):
            start = time.time()
            # (in pieces, each with a fresh token, going on from what is there: the tokens last an hour and the
            # connection sometimes slows to a crawl)
            part = file + '.part'
            total = None
            for attempt in range(200):
                have = os.path.getsize(part) if os.path.exists(part) else 0
                if total is not None and have >= total:
                    break
                try:
                    token = json.load(urllib.request.urlopen(TOKEN))['token']
                    req = urllib.request.Request(it['assets']['image']['href'] + '?' + token,
                                                 headers={'Range': f'bytes={have}-{have + 64 * 1024 * 1024 - 1}'})
                    with urllib.request.urlopen(req, timeout=60) as r, open(part, 'ab') as f:
                        rng = r.headers.get('Content-Range', '')
                        if '/' in rng:
                            total = int(rng.rsplit('/', 1)[1])
                        while True:
                            chunk = r.read(1 << 20)
                            if not chunk:
                                break
                            f.write(chunk)
                except OSError as e:
                    print(f'  {name}: {e} (try {attempt + 1})', flush=True)
                    time.sleep(3)
            if total is None or os.path.getsize(part) != total:
                sys.exit('cannot download ' + name)
            os.replace(part, file)
            print(f'[{k + 1}/{len(items)}] {name}: {os.path.getsize(file) / 1e6:.0f} MB, {time.time() - start:.0f} s', flush=True)
        with open(listing, 'w') as f:
            json.dump(known, f, indent=1)

def raster(o):
    import numpy as np
    import pyproj, tifffile, zarr
    w, p = load_world(o['world'])
    ax0, ay0, ax1, ay1 = area_of(o, w)
    cell = float(o.get('cell', 0.25))
    size = float(o.get('tile', 256))
    n = int(round(size / cell))
    out = o['out']
    os.makedirs(out, exist_ok=True)
    folder = o['images']
    known = json.load(open(os.path.join(folder, 'items.json')))
    pictures = []
    for name, it in sorted(known.items()):
        file = os.path.join(folder, name)
        if not os.path.exists(file):
            continue
        tif = tifffile.TiffFile(file)
        z = zarr.open(tif.series[0].aszarr(level=0), mode='r')
        t = it['transform'] # x = t0 + col * t1, y = t3 + row * t5 (north up)
        tr = pyproj.Transformer.from_crs(6318, it['epsg'], always_xy=True)
        pictures.append((name, tif, z, t, tr))
    if not pictures:
        sys.exit('no pictures in ' + folder)
    i0, i1 = int(np.floor(ax0 / size)), int(np.ceil(ax1 / size))
    j0, j1 = int(np.floor(ay0 / size)), int(np.ceil(ay1 / size))
    centres = (np.arange(n) + 0.5) * cell
    start = time.time()
    count = 0
    for tj in range(j0, j1):
        for ti in range(i0, i1):
            xs = ti * size + centres
            ys = tj * size + centres
            gx, gy = np.meshgrid(xs, ys) # rows south to north
            lat, lon = to_latlon(p, gx, gy)
            pixels = np.zeros((n, n, 4), np.uint8)
            filled = np.zeros((n, n), bool)
            for name, tif, z, t, tr in pictures:
                ex, ny = tr.transform(lon.ravel(), lat.ravel())
                col = (np.asarray(ex) - t[2]) / t[0] - 0.5
                row = (np.asarray(ny) - t[5]) / t[4] - 0.5
                col, row = col.reshape(n, n), row.reshape(n, n)
                h, wd = z.shape[0], z.shape[1]
                inside = (col >= 0) & (row >= 0) & (col < wd - 1) & (row < h - 1) & ~filled
                if not inside.any():
                    continue
                r0, r1 = int(np.floor(row[inside].min())), int(np.ceil(row[inside].max())) + 2
                c0, c1 = int(np.floor(col[inside].min())), int(np.ceil(col[inside].max())) + 2
                window = np.asarray(z[r0:min(r1, h), c0:min(c1, wd)]).astype(np.float32)
                rr, cc = row[inside] - r0, col[inside] - c0
                ri, ci = np.floor(rr).astype(np.int64), np.floor(cc).astype(np.int64)
                ri = np.clip(ri, 0, window.shape[0] - 2)
                ci = np.clip(ci, 0, window.shape[1] - 2)
                fr, fc = (rr - ri)[:, None], (cc - ci)[:, None]
                v = (window[ri, ci] * (1 - fr) * (1 - fc) + window[ri, ci + 1] * (1 - fr) * fc +
                     window[ri + 1, ci] * fr * (1 - fc) + window[ri + 1, ci + 1] * fr * fc)
                pixels[inside] = np.clip(v + 0.5, 0, 255).astype(np.uint8)[:, :4]
                filled |= inside
            if not filled.any():
                continue
            with open(os.path.join(out, f'a_{ti}_{tj}.bin'), 'wb') as f:
                f.write(b'WBAI' + struct.pack('<Ifff3I', 1, ti * size, tj * size, cell, n, n, 4))
                f.write(pixels.tobytes())
            count += 1
    print(f'{count} tiles in {out} ({time.time() - start:.0f} s)')

if __name__ == '__main__':
    if len(sys.argv) < 2 or sys.argv[1] not in ('fetch', 'raster'):
        sys.exit(__doc__)
    o = options(sys.argv[2:])
    (fetch if sys.argv[1] == 'fetch' else raster)(o)
