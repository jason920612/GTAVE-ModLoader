"""Land cover for the world builder's sectors (research/phase0.md §62): the USGS Annual National Land Cover Database
(NLCD; public domain; 30 m classes in CONUS Albers, EPSG:5070) from MRLC's coverage service, cut to a sector's box and
resampled onto the sector's game coordinates - what its woods, wetlands, fields and built-up land really are, where the
map (OpenStreetMap) leaves most of the countryside untagged.

usage: python -I tools/worldgen/nlcd.py --world <world.json> --out <landcover.bin> [--year 2024] [--cell 10]
       [--cache <dir>] [--margin 300]
  landcover.bin ('WBLC' version 1): x0, y0, cell (float32), nx, ny (uint32); then each cell's NLCD class (uint8: 11
  open water, 21-24 developed open space / low / medium / high intensity, 31 barren, 41 deciduous, 42 evergreen, 43 mixed
  forest, 52 shrub, 71 grassland, 81 pasture, 82 crops, 90 woody wetlands, 95 emergent herbaceous wetlands; 0 none),
  rows south to north.
The coverage is asked for once per sector box (WCS 1.0.0 GetCoverage, GeoTIFF at its own 30 m grid) and kept in --cache.
"""
import io, json, math, os, struct, sys, time, urllib.request

import numpy as np
from PIL import Image

SERVICE = 'https://dmsdata.cr.usgs.gov/geoserver/mrlc_Land-Cover-Native_conus_year_data/wcs'
COVERAGE = 'mrlc_Land-Cover-Native_conus_year_data:Land-Cover-Native_conus_year_data'
GRID_X0, GRID_Y0, PIXEL = -2415585.0, 3314805.0, 30.0  # (the coverage's grid: its north-west corner, its cells)

# CONUS Albers equal area (EPSG:5070: NAD83, standard parallels 29.5 and 45.5 N, origin 23 N 96 W)
_A, _F = 6378137.0, 1 / 298.257222101
_E2 = 2 * _F - _F * _F
_E = math.sqrt(_E2)


def _q(s):
    return (1 - _E2) * (s / (1 - _E2 * s * s) - (1 / (2 * _E)) * np.log((1 - _E * s) / (1 + _E * s)))


def _m(p):
    s = math.sin(p)
    return math.cos(p) / math.sqrt(1 - _E2 * s * s)


_P1, _P2, _P0, _L0 = (math.radians(v) for v in (29.5, 45.5, 23.0, -96.0))
_N = (_m(_P1) ** 2 - _m(_P2) ** 2) / (_q(math.sin(_P2)) - _q(math.sin(_P1)))
_C = _m(_P1) ** 2 + _N * _q(math.sin(_P1))
_RHO0 = _A * math.sqrt(_C - _N * _q(math.sin(_P0))) / _N


def albers(lat, lon):
    """(lat, lon) degrees (arrays or scalars) -> EPSG:5070 metres."""
    rho = _A * np.sqrt(_C - _N * _q(np.sin(np.radians(lat)))) / _N
    theta = _N * (np.radians(lon) - _L0)
    return rho * np.sin(theta), _RHO0 - rho * np.cos(theta)


def options(argv):
    o = {'year': '2024', 'cell': '10', 'cache': r'D:\GTAV-UE5\nlcd\cache', 'margin': '300'}
    for i in range(0, len(argv) - 1, 2):
        if not argv[i].startswith('--'):
            sys.exit(__doc__)
        o[argv[i][2:]] = argv[i + 1]
    if 'world' not in o or 'out' not in o:
        sys.exit(__doc__)
    return o


def fetch(x0, y0, x1, y1, year, cache):
    """The coverage's classes over the Albers box (on its grid), rows north to south."""
    os.makedirs(cache, exist_ok=True)
    width, height = int(round((x1 - x0) / PIXEL)), int(round((y1 - y0) / PIXEL))
    file = os.path.join(cache, f'nlcd_{year}_{int(x0)}_{int(y0)}_{width}x{height}.tif')
    if not os.path.exists(file):
        url = (f'{SERVICE}?service=WCS&version=1.0.0&request=GetCoverage&coverage={COVERAGE}&crs=EPSG:5070'
               f'&bbox={x0:.0f},{y0:.0f},{x1:.0f},{y1:.0f}&width={width}&height={height}&format=GeoTIFF'
               f'&TIME={year}-01-01T00:00:00.000Z')
        for attempt in range(5):
            try:
                with urllib.request.urlopen(url, timeout=300) as r:
                    data = r.read()
                if not data.startswith((b'II*\x00', b'MM\x00*')):
                    raise IOError(data[:300].decode('utf-8', 'replace'))
                break
            except Exception as e:  # (the service is busy now and then: tried again)
                if attempt == 4:
                    raise
                print(f'  land cover: {e}; again', flush=True)
                time.sleep(10 * (attempt + 1))
        with open(file + '.part', 'wb') as f:
            f.write(data)
        os.replace(file + '.part', file)
    im = Image.open(file)
    a = np.array(im)
    if a.shape != (height, width):
        raise IOError(f'{file}: {a.shape} for {height} x {width}')
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
    # each cell's middle -> latitude, longitude -> Albers
    xs = x0 + (np.arange(nx) + 0.5) * cell
    ys = y0 + (np.arange(ny) + 0.5) * cell
    lons = lon0 + (xs - gx0) / kx
    lats = lat0 + (ys - gy0) / ky
    lat, lon = np.meshgrid(lats, lons, indexing='ij')
    ax, ay = albers(lat, lon)
    # the coverage's box over them, on its grid (a pixel more round it)
    bx0 = GRID_X0 + (math.floor((ax.min() - GRID_X0) / PIXEL) - 1) * PIXEL
    bx1 = GRID_X0 + (math.ceil((ax.max() - GRID_X0) / PIXEL) + 1) * PIXEL
    by1 = GRID_Y0 - (math.floor((GRID_Y0 - ay.max()) / PIXEL) - 1) * PIXEL
    by0 = GRID_Y0 - (math.ceil((GRID_Y0 - ay.min()) / PIXEL) + 1) * PIXEL
    classes = fetch(bx0, by0, bx1, by1, o['year'], o['cache'])
    col = np.clip(((ax - bx0) / PIXEL).astype(np.int64), 0, classes.shape[1] - 1)
    row = np.clip(((by1 - ay) / PIXEL).astype(np.int64), 0, classes.shape[0] - 1)
    out = classes[row, col].astype(np.uint8)  # (rows south to north, as ys)
    with open(o['out'] + '.part', 'wb') as f:
        f.write(b'WBLC' + struct.pack('<I3f2I', 1, x0, y0, cell, nx, ny))
        f.write(out.tobytes())
    os.replace(o['out'] + '.part', o['out'])
    values, counts = np.unique(out, return_counts=True)
    share = ', '.join(f'{v}: {100 * c / out.size:.0f}%' for v, c in sorted(zip(values, counts), key=lambda t: -t[1])[:8])
    print(f'land cover {o["year"]}: {nx} x {ny} cells of {cell:.0f} m ({share})')


if __name__ == '__main__':
    main()
