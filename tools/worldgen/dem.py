"""Ground heights for the world builder's sectors (research/phase0.md §62): the USGS 3D Elevation Program's bare-earth
elevation (3DEP; public domain; metres above NAVD88, about the mean sea level in Florida; the best data there is,
hydro-flattened: a lake's surface one height, a river's stepping down along it) from The National Map's image service,
cut to a sector's box and resampled onto the sector's game coordinates.

usage: python -I tools/worldgen/dem.py --world <world.json> --out <ground.bin> [--cell 8] [--step 10] [--cache <dir>] [--margin 300]
  ground.bin ('WBGR' version 1): x0, y0, cell (float32), nx, ny (uint32); then each cell's height in centimetres (int16;
  -32768: not known - out at sea, where the service has nothing), rows south to north.
  --step: the metres a pixel asked for (10: a third of an arc second, the 10 m DEM's; 2: the lidar's 1 m DEM where the
          service has it - Florida's towns -, the yards over the streets, the ditches, the rises the street pictures show)
The service is asked for the sector box (exportImage, float32 GeoTIFF on a latitude / longitude grid; in tiles of at
most 2000 pixels a side) and the answers kept in --cache.
"""
import io, json, math, os, struct, sys, time, urllib.request

import numpy as np
from PIL import Image

SERVICE = 'https://elevation.nationalmap.gov/arcgis/rest/services/3DEPElevation/ImageServer/exportImage'
STEP = 1 / 10800  # (degrees a pixel asked for by default: a third of an arc second, about 10 m)
TILE = 2000  # (pixels a side a request at most: the service answers 500 to 4000 x 4000 of float32)


def options(argv):
    o = {'cell': '8', 'cache': r'D:\GTAV-UE5\dem\cache', 'margin': '300', 'step': ''}
    for i in range(0, len(argv) - 1, 2):
        if not argv[i].startswith('--'):
            sys.exit(__doc__)
        o[argv[i][2:]] = argv[i + 1]
    if 'world' not in o or 'out' not in o:
        sys.exit(__doc__)
    return o


def fetch(south, west, north, east, cache, step=STEP):
    """Heights over the box (metres, NaN where none), rows north to south, and the box as asked for (in tiles)."""
    width, height = int(math.ceil((east - west) / step)), int(math.ceil((north - south) / step))
    a = np.full((height, width), np.nan, dtype=np.float32)
    top = south + height * step
    for ty in range(0, height, TILE):
        for tx in range(0, width, TILE):
            w_, h_ = min(TILE, width - tx), min(TILE, height - ty)
            a[ty:ty + h_, tx:tx + w_] = fetch_one(top - (ty + h_) * step, west + tx * step, w_, h_, cache, step)
    return a, (south, west, top, west + width * step)


def fetch_one(south, west, width, height, cache, step):
    """One request's heights: width x height pixels of `step` degrees from the south-west corner, rows north to south."""
    os.makedirs(cache, exist_ok=True)
    tag = '' if step == STEP else f'_{1 / step:.0f}'
    file = os.path.join(cache, f'dem_{south:.4f}_{west:.4f}_{width}x{height}{tag}.tif')
    # (a kept answer that does not read whole - an earlier download cut short - asked for again)
    if os.path.exists(file):
        try:
            Image.open(file).load()
        except Exception:
            os.remove(file)
    if not os.path.exists(file):
        url = (f'{SERVICE}?bbox={west:.7f},{south:.7f},{west + width * step:.7f},{south + height * step:.7f}&bboxSR=4326&imageSR=4326'
               f'&size={width},{height}&format=tiff&pixelType=F32&interpolation=RSP_BilinearInterpolation&f=image')
        for attempt in range(6):
            try:
                with urllib.request.urlopen(url, timeout=600) as r:
                    data = r.read()
                if not data.startswith((b'II*\x00', b'MM\x00*')):
                    raise IOError(data[:300].decode('utf-8', 'replace'))
                # (all of it: the service now and then cuts an answer short - a TIFF's head and part of its pixels)
                Image.open(io.BytesIO(data)).load()
                break
            except Exception as e:  # (the service is busy now and then: tried again)
                if attempt == 5:
                    raise
                print(f'  ground: {e}; again', flush=True)
                time.sleep(15 * (attempt + 1))
        with open(file + '.part', 'wb') as f:
            f.write(data)
        os.replace(file + '.part', file)
    a = np.array(Image.open(file), dtype=np.float32)
    if a.shape != (height, width):
        raise IOError(f'{file}: {a.shape} for {height} x {width}')
    a[(a < -500) | (a > 9000) | ~np.isfinite(a)] = np.nan
    return a


def main():
    o = options(sys.argv[1:])
    w = json.load(open(o['world'], encoding='utf-8'))
    p = w['projection']
    lat0, lon0, gx0, gy0, kx, ky = (p[k] for k in ('lat0', 'lon0', 'x0', 'y0', 'kx', 'ky'))
    cell, margin = float(o['cell']), float(o['margin'])
    x0, y0 = w['min'][0] - margin, w['min'][1] - margin
    nx = int(math.ceil((w['max'][0] + margin - x0) / cell))
    ny = int(math.ceil((w['max'][1] + margin - y0) / cell))
    xs = x0 + (np.arange(nx) + 0.5) * cell
    ys = y0 + (np.arange(ny) + 0.5) * cell
    lons = lon0 + (xs - gx0) / kx
    lats = lat0 + (ys - gy0) / ky
    step = float(o['step']) / 111320 if o['step'] else STEP
    pad = 3 * step
    a, (s, wl, n, e) = fetch(lats.min() - pad, lons.min() - pad, lats.max() + pad, lons.max() + pad, o['cache'], step)
    # bilinear, by pixel centres (row 0 the northmost)
    fx = (lons - wl) / step - 0.5
    fy = (n - lats) / step - 0.5
    ix = np.clip(np.floor(fx).astype(np.int64), 0, a.shape[1] - 2)
    iy = np.clip(np.floor(fy).astype(np.int64), 0, a.shape[0] - 2)
    tx = np.clip(fx - ix, 0, 1)[None, :]
    ty = np.clip(fy - iy, 0, 1)[:, None]
    Y, X = np.meshgrid(iy, ix, indexing='ij')
    v00, v01, v10, v11 = a[Y, X], a[Y, X + 1], a[Y + 1, X], a[Y + 1, X + 1]
    h = (v00 * (1 - tx) + v01 * tx) * (1 - ty) + (v10 * (1 - tx) + v11 * tx) * ty
    # (a pixel without data next to ones with: the nearest of the four that has)
    near = np.where(np.isnan(v00), np.where(np.isnan(v01), np.where(np.isnan(v10), v11, v10), v01), v00)
    h = np.where(np.isnan(h), near, h)
    cm = np.where(np.isnan(h), -32768, np.clip(np.round(h * 100), -32767, 32767)).astype(np.int16)
    with open(o['out'] + '.part', 'wb') as f:
        f.write(b'WBGR' + struct.pack('<I3f2I', 1, x0, y0, cell, nx, ny))
        f.write(cm.tobytes())
    os.replace(o['out'] + '.part', o['out'])
    known = h[~np.isnan(h)]
    if known.size:
        print(f'ground: {nx} x {ny} cells of {cell:.0f} m, {100 * known.size / h.size:.0f}% known, '
              f'{known.min():.1f}..{known.max():.1f} m (median {np.median(known):.1f})')
    else:
        print(f'ground: {nx} x {ny} cells of {cell:.0f} m, none known (all sea)')


if __name__ == '__main__':
    main()
