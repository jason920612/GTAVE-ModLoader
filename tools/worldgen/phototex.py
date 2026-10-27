"""Street panoramas on the walls (research/phase0.md §63.3): the downloads' 360-degree pictures (Mapillary, CC BY-SA 4.0:
their creators named in the output) put on our buildings' walls as their textures, so that a street the pictures saw
looks as they show it.

usage: python -I tools/worldgen/phototex.py --world <world.json> --metadata <metadata.csv>[,...] --scan <raster dir>
           --ground <ground.bin> --out <dir> [--area x0,y0,x1,y1] [--ids a,b,...] [--workers 12] [--density 16]
           [--check N] [--redo 1] [--ground 1]

Each picture's place, heading, camera height and tilt first (a phone's or a car's position is metres off, its compass
degrees): its skyline - per column how far down the sky reaches - against the one the laser scan's top surface (lidar.py
raster: roofs and crowns, a few years older) has from a place: the place within 8 m of where the picture says (1 m steps,
then a quarter), the camera 1.5-3.3 m over the measured ground, the heading within 40 degrees (to a quarter of one), the
tilt a sine round the horizon (a car's camera - CARS, the creators who drove - kept on the map's carriageways: off them
0.15 degree a square metre; the heading 0.001 a square degree from the picture's own); the columns' differences capped at
5 degrees (trees grown or cut, cars, wires, buildings
built since). A picture whose skyline then misses by more than --miss degrees (1.0) in the median over the columns that
show something, or agrees within 1.5 degrees in less than 60 % of them, is not used (under a canopy no fit is possible). The fits are kept (<dir>/poses.json) and reused unless --redo.

Then each wall of each building (world.json footprints, anticlockwise) a picture within 80 m faces: a texture over the
wall from 0.6 m under its lowest ground to its building's top (the scan's highest building return over the footprint,
else the map's height), at most --density texels a metre (as many as its best picture has there). Each texel from the
pictures that see it - nothing in the way on the scan's surface (buildings solid, a tree's crown its top two-thirds, the
last 1.2 m before the wall not looked at), under the sky's edge, above the car's roof -: per half-metre block the picture
that agrees most with the others that see it (a car or a person in one is in no other), else the nearest and most
frontal; the pictures' exposures matched to the wall's main one, the blocks blended into each other over a block; what
no picture sees filled from round it (first one storey up or down where the wall repeats by storeys). Walls less than a third
of which some picture sees are left out, and those that fail the checks: at most half of it leaves or shade (green with a
busy texture, or dark); its top where the main picture's skyline is (within 1.5 degrees, half its columns) where its top
is the skyline on the scan, else the main picture within 30 m or two pictures agreeing on it; no more than a tenth of it
sky in the main picture between a metre over the ground and a metre under its top; the scan's building returns a metre
inside it and none 1.5 m out along three fifths of it; three fifths of it the pictures' own texels, not the fill.

--ground 1: the ground round the pictures too, on the build's own surface there (worldgen build --phototex <dir> writes
<dir>/surface.bin: the roads', sidewalks' and paving's tops over the terrain, every 0.1 m in 25.6 m blocks within 16 m of
a picture): the decal's heights every 0.8 m (the highest surface within 0.45 m, 6 cm over it - the next build lays the
pictures on those, so that from where a picture was taken its ground is drawn as it shows it, whatever the build's
ground is), each 0.1 m texel from the nearest pictures that see it (within 16 m, outside the car, nothing on the scan's
surface in the way; per 0.8 m cell the nearest's unless two others agree on something else - a car, a person, a shadow);
of that only the paint is kept (white and yellow marks over the asphalt round them, thin: lines, bars, letters) in clean
paint colours, alpha 1 where the pictures see the ground and nothing is painted (the build leaves out its own markings
there), 0 where none sees it: <dir>/ground.json, <dir>/ground.bin.

Out: <dir>/phototex.json - the pictures (place, heading, fit, creator) and the walls (building, edge, its ends, z0, z1,
the texture's size and its offset in walls.bin, the pictures used) -; <dir>/walls.bin - the textures, RGB, rows from the
top (z1) down, columns from the edge's first end; --check N: <dir>/check_<id>.jpg for N pictures - the scan's skyline
over the picture where it said it was (red) and fitted (green) - and <dir>/walls_<k>.jpg, some walls' textures.
"""
import csv, json, math, os, struct, sys, time
from multiprocessing import Pool

import numpy as np
from PIL import Image, ImageDraw

CELL = 0.5        # the scan's cells (m)
BINS = 1440       # skyline columns: a quarter of a degree
CAP = 5.0         # a column's difference counts at most this (degrees)
VEHICLE = -25.0   # below this elevation the car's roof (degrees)
REACH = 80.0      # walls this near a picture (m)
# (the creators whose panoramas are from a car's roof - a car under the camera in all of them; the others walked)
CARS = {'maxolasersquad', 'bchamberlain', 'ochunter', 'COT_UUPI'}
DATUM = 0.2       # the build's buildings stand this much over the scan (worldgen build --datum): the walls' pictures are taken
                  # of them there, so that from a picture's place - on the build's ground, which is the measured one - they are
                  # drawn as it shows them
BLOCK = 0.5       # the texture's blocks (m)
# (the ray's samples: every half metre to 60 m, two to 150, five to 400 - a tower far off stands over the trees)
DISTS = np.concatenate([np.arange(1.0, 60, 0.5), np.arange(60, 150, 2.0), np.arange(150, 400, 5.0)]).astype(np.float32)


def options(argv):
    o = {}
    for i in range(0, len(argv) - 1, 2):
        if not argv[i].startswith('--'):
            sys.exit(__doc__)
        o[argv[i][2:]] = argv[i + 1]
    return o


# ---------------------------------------------------------------------------------------------------------------- data

def road_raster(world, box):
    """Per metre over the box (rows from the north): how far it is off the map's carriageways (their lanes' width)."""
    from PIL import ImageDraw
    from scipy import ndimage
    x0, y0, x1, y1 = box
    W, H = int(x1 - x0), int(y1 - y0)
    im = Image.new('L', (W, H), 0)
    dr = ImageDraw.Draw(im)
    for r in world.get('roads', []):
        if r.get('kind') in ('footway', 'path', 'cycleway', 'steps', 'pedestrian', 'track', 'bridleway') or len(r.get('points', [])) < 2:
            continue
        half = float(r.get('laneWidth', 3.5)) * max(1, int(r.get('forward', 1)) + int(r.get('backward', 1))) / 2
        dr.line([(p[0] - x0, y1 - p[1]) for p in r['points']], fill=255, width=max(1, int(round(2 * half))))
    return ndimage.distance_transform_edt(np.asarray(im) == 0).astype(np.float16)


def make_cache(scan_dir, ground_path, box, cache, world=None):
    """The scan's layers over the box as one mosaic (top, roof: cm, -32768 none; flags) and the measured ground cut to it,
    as .npy files the workers map. The top surface opened by 1.5 m: the wires' and the poles' returns - lines and points a
    cell wide - had drawn arcs and spikes over the street's sky that the pictures' skylines do not have."""
    os.makedirs(cache, exist_ok=True)
    meta_path = os.path.join(cache, 'meta.json')
    if os.path.exists(meta_path):
        with open(meta_path, encoding='utf-8') as f:
            m = json.load(f)
        if m.get('box') == list(box) and m.get('opened') == 3 and m.get('roads') == 1:
            return m
    files = [f for f in os.listdir(scan_dir) if f.startswith('t_') and f.endswith('.bin')]
    with open(os.path.join(scan_dir, files[0]), 'rb') as f:
        _, _, _, _, cell, tw, th, _ = struct.unpack('<4sIfffIII', f.read(32))
    tile = cell * tw
    i0, j0 = int(math.floor(box[0] / tile)), int(math.floor(box[1] / tile))
    i1, j1 = int(math.floor(box[2] / tile)), int(math.floor(box[3] / tile))
    nx, ny = (i1 - i0 + 1) * tw, (j1 - j0 + 1) * th
    top = np.full((ny, nx), -32768, np.int16)
    roof = np.full((ny, nx), -32768, np.int16)
    flags = np.zeros((ny, nx), np.uint8)
    for j in range(j0, j1 + 1):
        for i in range(i0, i1 + 1):
            p = os.path.join(scan_dir, f't_{i}_{j}.bin')
            if not os.path.exists(p):
                continue
            with open(p, 'rb') as f:
                _, _, _, _, _, w, h, _ = struct.unpack('<4sIfffIII', f.read(32))
                n = w * h
                t = np.frombuffer(f.read(2 * n), '<i2').reshape(h, w)
                f.read(2 * n)  # (the ground's mean: the measured ground below is whole)
                r = np.frombuffer(f.read(2 * n), '<i2').reshape(h, w)
                f.read(n)
                fl = np.frombuffer(f.read(n), np.uint8).reshape(h, w)
            oy, ox = (j - j0) * th, (i - i0) * tw
            top[oy:oy + h, ox:ox + w] = t
            roof[oy:oy + h, ox:ox + w] = r
            flags[oy:oy + h, ox:ox + w] = fl
    with open(ground_path, 'rb') as f:
        head = f.read(28)
        if head[:4] != b'WBGR':
            sys.exit(f'{ground_path}: not a ground.bin')
        _, gx, gy, gc, gnx, gny = struct.unpack('<Ifff2I', head[4:])
        dem = np.frombuffer(f.read(2 * gnx * gny), '<i2').reshape(gny, gnx)
    # (the ground cut to the box: whole cells)
    a0, b0 = max(0, int((box[0] - gx) / gc)), max(0, int((box[1] - gy) / gc))
    a1, b1 = min(gnx, int((box[2] - gx) / gc) + 2), min(gny, int((box[3] - gy) / gc) + 2)
    from scipy import ndimage
    top = ndimage.grey_opening(top, size=(3, 3))
    np.save(os.path.join(cache, 'top.npy'), top)
    np.save(os.path.join(cache, 'roof.npy'), roof)
    np.save(os.path.join(cache, 'flags.npy'), flags)
    np.save(os.path.join(cache, 'dem.npy'), np.ascontiguousarray(dem[b0:b1, a0:a1]))
    np.save(os.path.join(cache, 'offroad.npy'), road_raster(world or {}, box))
    m = {'box': list(box), 'opened': 3, 'roads': 1, 'x0': i0 * tile, 'y0': j0 * tile, 'cell': cell, 'gx': gx + a0 * gc, 'gy': gy + b0 * gc, 'gc': gc}
    with open(meta_path, 'w', encoding='utf-8') as f:
        json.dump(m, f)
    return m


class Data:
    """The worker's view of the cache."""

    def __init__(self, cache):
        with open(os.path.join(cache, 'meta.json'), encoding='utf-8') as f:
            self.m = json.load(f)
        self.top = np.load(os.path.join(cache, 'top.npy'), mmap_mode='r')
        self.roof = np.load(os.path.join(cache, 'roof.npy'), mmap_mode='r')
        self.flags = np.load(os.path.join(cache, 'flags.npy'), mmap_mode='r')
        self.dem = np.load(os.path.join(cache, 'dem.npy'), mmap_mode='r')
        self.offroad = np.load(os.path.join(cache, 'offroad.npy'), mmap_mode='r')

    def cells(self, x, y):
        ny, nx = self.top.shape
        i = np.floor((x - self.m['x0']) / self.m['cell']).astype(np.int64)
        j = np.floor((y - self.m['y0']) / self.m['cell']).astype(np.int64)
        ok = (i >= 0) & (i < nx) & (j >= 0) & (j < ny)
        return np.clip(i, 0, nx - 1), np.clip(j, 0, ny - 1), ok

    def surface(self, x, y):
        """The scan's top surface (m; nan where none)."""
        i, j, ok = self.cells(x, y)
        t = self.top[j, i].astype(np.float32)
        return np.where(ok & (t > -32000), t / 100, np.nan)

    def off_road(self, x, y):
        """How far off the map's carriageways (m)."""
        H, W = self.offroad.shape
        i = int(np.clip(x - self.m['box'][0], 0, W - 1))
        j = int(np.clip(self.m['box'][3] - y, 0, H - 1))
        return float(self.offroad[j, i])

    def ground(self, x, y):
        """The measured ground (m; nan where none)."""
        ny, nx = self.dem.shape
        i = np.floor((np.asarray(x) - self.m['gx']) / self.m['gc']).astype(np.int64)
        j = np.floor((np.asarray(y) - self.m['gy']) / self.m['gc']).astype(np.int64)
        ok = (i >= 0) & (i < nx) & (j >= 0) & (j < ny)
        g = self.dem[np.clip(j, 0, ny - 1), np.clip(i, 0, nx - 1)].astype(np.float32)
        return np.where(ok & (g > -32000), g / 100, np.nan)


_data = None
_images = {}


def worker_init(cache):
    global _data
    _data = Data(cache)


def image(path):
    """A panorama (uint8), the last few kept."""
    if path not in _images:
        if len(_images) >= 40:
            _images.pop(next(iter(_images)))
        _images[path] = np.asarray(Image.open(path).convert('RGB'))
    return _images[path]


# ------------------------------------------------------------------------------------------------------------ the fit

def pano_skyline(img):
    """Per column bin (BINS round the picture from its back, -180 degrees off its heading), the elevation (degrees) the
    sky reaches down to: the first pixel from the top that is not sky with less than half sky in the 3 degrees under it -
    wires (the bundles near the camera thick enough to count), branches and signs have sky under them, a building's or a
    crown's top has not (more than 3 degrees: a far crown is no taller) -, at most 8 degrees under the horizon; then the median over 1.75 degrees (a pole's column, a gap
    in the leaves). Sky: bluish and bright, or near white."""
    from scipy import ndimage
    H, W = img.shape[:2]
    lim = int(H * (0.5 + 8 / 180))
    a = img[:lim].astype(np.int16)
    r, g, b = a[..., 0], a[..., 1], a[..., 2]
    mx, mn = a.max(-1), a.min(-1)
    sat = (mx - mn) / np.maximum(mx, 1)
    sky = ((b >= r) & (b >= g - 8) & (mx > 110)) | ((mx > 190) & (sat < 0.12))
    K = int(H * 3 / 180)
    C = np.concatenate([np.zeros((1, W), np.int32), np.cumsum(sky, 0, dtype=np.int32)])
    rows = np.arange(lim)
    hi = np.minimum(rows + K, lim)
    frac = (C[hi] - C[rows]) / (hi - rows)[:, None]
    solid = ~sky & (frac < 0.5)
    row = np.where(solid.any(0), solid.argmax(0), lim - 1)
    el = 90 - (row + 0.5) * 180 / H
    m = ((np.arange(W) + 0.5) / W * BINS).astype(np.int64)
    s, n = np.bincount(m, weights=el, minlength=BINS), np.bincount(m, minlength=BINS)
    out = s / np.maximum(n, 1)
    if (n == 0).any():
        k = np.nonzero(n)[0]
        out = np.interp(np.arange(BINS), k, out[k], period=BINS)
    return ndimage.median_filter(out, size=7, mode='wrap')


def model_tops(d, x, y, az):
    """The scan's top surface along rays from (x, y) towards compass `az` (degrees) at DISTS: (len(az), len(DISTS))."""
    ax, ay = np.sin(np.radians(az)), np.cos(np.radians(az))
    return d.surface(x + ax[:, None] * DISTS[None, :], y + ay[:, None] * DISTS[None, :])


def model_skyline(tops, zc):
    """The highest elevation along each ray (degrees), the median over 1.75 degrees as the pictures' are."""
    from scipy import ndimage
    s = (tops - zc) / DISTS[None, :]
    e = np.degrees(np.arctan(np.nanmax(np.where(np.isnan(s), -1.0, s), axis=1)))
    return ndimage.median_filter(e, size=7, mode='wrap')


def search(d, ep, h0, x0, y0, offsets, heights, shifts, nb, prior_from, on_road=False):
    """The best (cost, x, y, height over ground, shift) over places (x0, y0) + offsets, camera heights, heading shifts
    (bins of 360 / nb degrees from h0): the skyline `ep` (nb bins from h0 - 180) against the scan's."""
    az = h0 - 180 + (np.arange(nb) + 0.5) * 360 / nb
    idx = (np.arange(nb)[None, :] + shifts[:, None]) % nb
    best = None
    for dx, dy in offsets:
        x, y = x0 + dx, y0 + dy
        zg = float(d.ground(x, y))
        if math.isnan(zg):
            continue
        tops = model_tops(d, x, y, az)
        prior = 0.004 * ((x - prior_from[0]) ** 2 + (y - prior_from[1]) ** 2)
        if on_road:
            # (a car's camera: on the carriageway - the map's, which the build draws)
            prior += 0.15 * d.off_road(x, y) ** 2
        for hc in heights:
            em = model_skyline(tops, zg + hc)
            # (a car's roof camera about 2.2 m up: the height otherwise took up the crowns' noise)
            cost = (np.minimum(np.abs(ep[None, :] - em[idx]), CAP).mean(1) + prior + 0.1 * ((hc - 2.2) / 0.5) ** 2 +
                    0.001 * (shifts * 360.0 / nb) ** 2)
            k = int(np.argmin(cost))
            if best is None or cost[k] < best[0]:
                best = (float(cost[k]), x, y, hc, int(shifts[k]), zg)
    return best


def register(job):
    """A panorama's place, heading, camera height and tilt (see the usage)."""
    d = _data
    img = image(job['path'])
    raw = ep = pano_skyline(img)
    gx, gy, h0 = job['x'], job['y'], job['heading']
    if math.isnan(float(d.ground(gx, gy))):
        return {**job, 'ok': False, 'why': 'no ground'}
    # (coarse: a metre, half a degree; two camera heights)
    ep2 = ep.reshape(BINS // 2, 2).mean(1)
    grid = [(dx, dy) for dx in np.arange(-8, 8.01, 1.0) for dy in np.arange(-8, 8.01, 1.0) if dx * dx + dy * dy <= 8.01 ** 2]
    car = job.get('creator') in CARS
    best = search(d, ep2, h0, gx, gy, grid, (1.8, 2.4), np.arange(-80, 81), BINS // 2, (gx, gy), car)
    if best is None:
        return {**job, 'ok': False, 'why': 'off the scan'}
    _, bx, by, _, bs, _ = best
    h = h0 + bs * 360 / (BINS // 2)
    tilt = (0.0, 0.0)
    fine = [(dx, dy) for dx in np.arange(-1.5, 1.51, 0.5) for dy in np.arange(-1.5, 1.51, 0.5)]
    finer = [(dx, dy) for dx in np.arange(-0.25, 0.26, 0.25) for dy in np.arange(-0.25, 0.26, 0.25)]
    heights = np.arange(1.5, 3.35, 0.2)
    for _ in range(2):
        az_rel = np.radians(-180 + (np.arange(BINS) + 0.5) * 360 / BINS)
        epc = ep - (tilt[0] * np.cos(az_rel) + tilt[1] * np.sin(az_rel))
        s0 = int(round((h - h0) * BINS / 360))
        best = search(d, epc, h0, bx, by, fine, heights, np.arange(s0 - 8, s0 + 9), BINS, (gx, gy), car)
        cost, bx, by, hc, s, zg = best
        best = search(d, epc, h0, bx, by, finer, heights, np.arange(s - 4, s + 5), BINS, (gx, gy), car)
        cost, bx, by, hc, s, zg = best
        h = h0 + s * 360 / BINS
        # (the tilt: what is left as a sine round the horizon, over the columns that agree)
        az = h0 - 180 + (np.arange(BINS) + 0.5) * 360 / BINS
        em = model_skyline(model_tops(d, bx, by, az), zg + hc)
        em = em[(np.arange(BINS) + s) % BINS]
        res = epc - em + (tilt[0] * np.cos(az_rel) + tilt[1] * np.sin(az_rel))
        use = (np.abs(epc - em) < 2) & (np.maximum(epc, em) > 1)
        if use.sum() > 60:
            A = np.stack([np.cos(az_rel[use]), np.sin(az_rel[use]), np.ones(use.sum())], 1)
            coef = np.linalg.lstsq(A, res[use], rcond=None)[0]
            tilt = (float(np.clip(coef[0], -4, 4)), float(np.clip(coef[1], -4, 4)))
    az_rel = np.radians(-180 + (np.arange(BINS) + 0.5) * 360 / BINS)
    epc = ep - (tilt[0] * np.cos(az_rel) + tilt[1] * np.sin(az_rel))
    diff = np.abs(epc - em)
    # (how well it fits: over the columns that show something - but not what is overhead: a crown over the camera, where
    # the sky is in no column and any place fits - the median difference and the share that agrees: the picture's skyline
    # within 1.5 degrees over the scan's or 3 under it - the scan has a crown's thin top that the picture's sky outshines)
    shows = (np.maximum(epc, em) > 1) & (np.maximum(epc, em) < 45)
    miss = float(np.median(diff[shows])) if shows.sum() > 200 else 99.0
    agree = float((((epc - em) < 1.5) & ((epc - em) > -3))[shows].mean()) if shows.sum() > 200 else 0.0
    return {**job, 'ok': True, 'fx': round(bx, 2), 'fy': round(by, 2), 'zc': round(zg + hc, 2), 'above': round(float(hc), 2),
            'fh': round(h % 360, 2), 'tilt': [round(tilt[0], 2), round(tilt[1], 2)], 'miss': round(miss, 3), 'agree': round(agree, 3),
            'moved': round(math.hypot(bx - gx, by - gy), 2), 'turned': round((h - h0 + 180) % 360 - 180, 2),
            'sky': [round(float(v), 2) for v in raw]}


def check_picture(pose, cache, out):
    """The scan's skyline over the picture: where it said it was (red), fitted (green)."""
    d = Data(cache)
    img = np.asarray(Image.open(pose['path']).convert('RGB'))
    H, W = img.shape[:2]
    pic = Image.fromarray(img).resize((W // 2, H // 2))
    dr = ImageDraw.Draw(pic)
    for (x, y, h, zc, tilt, colour) in ((pose['x'], pose['y'], pose['heading'], None, (0, 0), (255, 40, 40)),
                                        (pose['fx'], pose['fy'], pose['fh'], pose['zc'], pose['tilt'], (40, 255, 40))):
        if zc is None:
            zc = float(d.ground(x, y)) + 2.0
        az = h - 180 + (np.arange(BINS) + 0.5) * 360 / BINS
        em = model_skyline(model_tops(d, x, y, az), zc)
        az_rel = np.radians(-180 + (np.arange(BINS) + 0.5) * 360 / BINS)
        em = em + tilt[0] * np.cos(az_rel) + tilt[1] * np.sin(az_rel)
        pts = [((k + 0.5) / BINS * W / 2, (0.5 - em[k] / 180) * H / 2) for k in range(BINS)]
        dr.line(pts, fill=colour, width=2)
    # (the picture's own skyline, blue)
    sky = pose['sky']
    dr.line([((k + 0.5) / BINS * W / 2, (0.5 - sky[k] / 180) * H / 2) for k in range(BINS)], fill=(60, 120, 255), width=1)
    dr.rectangle((0, 0, 560, 16), fill=(0, 0, 0))
    dr.text((4, 2), f'{pose["id"]} moved {pose["moved"]} m, turned {pose["turned"]}, {pose["above"]} m up, tilt {pose["tilt"]}, '
                    f'miss {pose["miss"]}, agree {pose["agree"]}', fill=(255, 255, 255))
    pic.save(os.path.join(out, f'check_{pose["id"]}.jpg'), quality=88)


# ---------------------------------------------------------------------------------------------------------- the walls

def sample(img, px, py):
    """Bilinear, wrapping round in x."""
    H, W = img.shape[:2]
    x0 = np.floor(px).astype(np.int64)
    y0 = np.clip(np.floor(py).astype(np.int64), 0, H - 2)
    fx = (px - x0)[..., None].astype(np.float32)
    fy = np.clip(py - y0, 0, 1)[..., None].astype(np.float32)
    x0 %= W
    x1 = (x0 + 1) % W
    a, b = img[y0, x0].astype(np.float32), img[y0, x1].astype(np.float32)
    c, e = img[y0 + 1, x0].astype(np.float32), img[y0 + 1, x1].astype(np.float32)
    return (a * (1 - fx) + b * fx) * (1 - fy) + (c * (1 - fx) + e * fx) * fy


def direction(pose, X, Y, Z):
    """The picture's pixel (px, py) for world points, and their elevation in it (degrees; the tilt in)."""
    dx, dy = X - pose['fx'], Y - pose['fy']
    az_rel = (np.degrees(np.arctan2(dx, dy)) - pose['fh'] + 180) % 360 - 180
    el = np.degrees(np.arctan2(Z - pose['zc'], np.hypot(dx, dy)))
    r = np.radians(az_rel)
    el = el + pose['tilt'][0] * np.cos(r) + pose['tilt'][1] * np.sin(r)
    return az_rel, el


def visibility(d, pose, bx, by, bz, a_n):
    """Per block (rows bz, columns at bx, by): whether the picture sees it - facing it, nothing on the scan's surface in
    the way, under the sky's edge, above the car's roof - and its weight (the cosine over the distance)."""
    cx, cy, cz = pose['fx'], pose['fy'], pose['zc']
    vx, vy = cx - bx, cy - by
    d2 = np.hypot(vx, vy)
    cos = (vx * a_n[0] + vy * a_n[1]) / np.maximum(d2, 1e-3)
    nb = len(bx)
    steps = np.arange(0.5, max(1.0, float(d2.max()) - 1.2), 0.5, dtype=np.float32)
    S = len(steps)
    t = steps[None, :] * np.ones((nb, 1), np.float32)
    live = t < (d2[:, None] - 1.2)
    ux, uy = -vx / np.maximum(d2, 1e-3), -vy / np.maximum(d2, 1e-3)
    sx, sy = cx + ux[:, None] * t, cy + uy[:, None] * t
    i, j, ok = d.cells(sx, sy)
    top = d.top[j, i].astype(np.float32) / 100
    roof = d.roof[j, i].astype(np.float32) / 100
    fl = d.flags[j, i]
    gnd = d.ground(sx, sy)
    gnd = np.where(np.isnan(gnd), top, gnd)
    valid = ok & live & (d.top[j, i] > -32000)
    building = (fl & 4) > 0
    veg = ((fl & 8) > 0) & ~building
    high = np.where(building & (d.roof[j, i] > -32000), roof, top)
    tall = veg & (top - gnd > 2.5)
    low = np.where(tall, gnd + 0.35 * (top - gnd), -1e9)
    high = np.where(valid, high, -1e9)
    # (the ray's height over each sample, per row)
    R = cz + (bz[:, None, None] - cz) * (t[None, :, :] / np.maximum(d2[None, :, None], 1e-3))
    blocked = ((R > low[None]) & (R < high[None] - 0.3)).any(-1) if S else np.zeros((len(bz), nb), bool)
    az_rel, el = direction(pose, bx[None, :], by[None, :], bz[:, None])
    k = (((az_rel + 180) / 360 * BINS).astype(np.int64)) % BINS
    sky = np.asarray(pose['sky'])[k]
    vis = (cos[None, :] > 0.17) & ~blocked & (el > VEHICLE) & (el < sky - 0.2)
    skyward = (cos[None, :] > 0.17) & (el >= sky - 0.2)
    d3 = np.hypot(d2[None, :], bz[:, None] - cz)
    weight = np.clip(cos, 0, 1)[None, :] / np.maximum(d3, 4)
    return vis, skyward, weight


def push_pull(img, mask):
    """Holes (mask False) filled from round them: the masked means of ever coarser levels, back down (bilinear: no blocks)."""
    from scipy import ndimage
    if mask.all() or not mask.any():
        return img
    H, W = mask.shape
    m = mask.astype(np.float32)
    c = img * m[..., None]
    h2, w2 = (H + 1) // 2, (W + 1) // 2
    pc = np.zeros((h2 * 2, w2 * 2, 3), np.float32)
    pm = np.zeros((h2 * 2, w2 * 2), np.float32)
    pc[:H, :W], pm[:H, :W] = c, m
    sc = pc.reshape(h2, 2, w2, 2, 3).sum((1, 3))
    sm = pm.reshape(h2, 2, w2, 2).sum((1, 3))
    mean = sc / np.maximum(sm, 1e-6)[..., None]
    coarse = push_pull(mean, sm > 0) if (h2 > 1 or w2 > 1) else mean
    up = ndimage.zoom(coarse, (2, 2, 1), order=1, mode='nearest')[:H, :W]
    return np.where(mask[..., None], img, up)


def storeys(tex, mask, rho):
    """What no picture sees filled one storey (or more) up or down from what one does, where the wall repeats by storeys:
    the lag of 2.6-4.6 m that differs least from itself, a fifth better than half a storey's."""
    H = mask.shape[0]
    if mask.all() or mask.sum() < 0.15 * mask.size:
        return tex, mask
    lum = tex.mean(-1)
    best, lag = None, 0
    for L in range(int(2.6 * rho), int(4.6 * rho) + 1):
        if L >= H:
            break
        both = mask[:-L] & mask[L:]
        if both.sum() < 0.08 * mask.size:
            continue
        e = float(np.abs(lum[:-L] - lum[L:])[both].mean())
        if best is None or e < best:
            best, lag = e, L
    half = max(1, int(1.5 * rho))
    if not lag or half >= H:
        return tex, mask
    both = mask[:-half] & mask[half:]
    if not both.any() or best > 0.8 * float(np.abs(lum[:-half] - lum[half:])[both].mean()):
        return tex, mask
    out, m = tex.copy(), mask.copy()
    for k in range(1, 16):
        for sgn in (1, -1):
            sh = sgn * k * lag
            if abs(sh) >= H:
                continue
            src, val = np.zeros_like(mask), np.zeros_like(tex)
            if sh > 0:
                src[:-sh], val[:-sh] = mask[sh:], tex[sh:]
            else:
                src[-sh:], val[-sh:] = mask[:sh], tex[:sh]
            fill = ~m & src
            out[fill] = val[fill]
            m |= fill
    return out, m


def texture_wall(w, poses, density):
    """One wall's texture (see the usage); None when too little of it is seen."""
    d = _data
    a, b = np.array(w['a'], np.float64), np.array(w['b'], np.float64)
    L = float(np.hypot(*(b - a)))
    u = (b - a) / L
    n = np.array([u[1], -u[0]])
    z0, z1 = w['z0'], w['z1']
    nbx, nby = max(1, int(math.ceil(L / BLOCK))), max(1, int(math.ceil((z1 - z0) / BLOCK)))
    bs = (np.arange(nbx) + 0.5) * L / nbx
    bz = z1 - (np.arange(nby) + 0.5) * (z1 - z0) / nby
    bx, by = a[0] + u[0] * bs + n[0] * 0.1, a[1] + u[1] * bs + n[1] * 0.1
    cands = []
    for pid in w['cands']:
        p = poses[pid]
        vis, skyward, weight = visibility(d, p, bx, by, bz, n)
        if vis.sum() >= 4:
            cands.append((pid, vis, skyward, weight))
    if not cands:
        return None
    # (the best few: by what they see, weighted; the first the wall's main picture)
    cands.sort(key=lambda c: -float((c[1] * c[3]).sum()))
    cands = cands[:6]
    seen = np.zeros((nby, nbx), bool)
    sky_any = np.zeros((nby, nbx), bool)
    for _, vis, skyward, _ in cands:
        seen |= vis
        sky_any |= skyward
    # (the share seen of the blocks above the ground no picture shows as sky - over the roof's edge)
    countable = (~sky_any | seen) & (bz[:, None] > z0 + 0.6)
    coverage = float(seen[countable].mean()) if countable.any() else 0.0
    if coverage < 0.33:
        return None
    # the texels: as many a metre as the best picture has (326 px a radian at 2048 wide)
    rho = 0.0
    for pid, vis, _, weight in cands:
        rho = max(rho, 326.0 * float(np.median(weight[vis])) if vis.any() else 0.0)
    rho = float(np.clip(round(rho), 6, density))
    TW, TH = max(4, int(math.ceil(L * rho))), max(4, int(math.ceil((z1 - z0) * rho)))
    ts = (np.arange(TW) + 0.5) / TW * L
    tz = z1 - (np.arange(TH) + 0.5) / TH * (z1 - z0)
    X, Y = a[0] + u[0] * ts + n[0] * 0.05, a[1] + u[1] * ts + n[1] * 0.05
    # (the blocks' values at the texels: bilinear between the blocks' middles)
    fx, fy = ts / (L / nbx) - 0.5, (z1 - tz) / ((z1 - z0) / nby) - 0.5
    fi, fj = np.floor(fx).astype(np.int64), np.floor(fy).astype(np.int64)
    i0, i1 = np.clip(fi, 0, nbx - 1), np.clip(fi + 1, 0, nbx - 1)
    j0, j1 = np.clip(fj, 0, nby - 1), np.clip(fj + 1, 0, nby - 1)
    wx, wy = (fx - fi).astype(np.float32), (fy - fj).astype(np.float32)

    def bilerp(G):
        G = G.astype(np.float32)
        return ((G[j0][:, i0] * (1 - wx) + G[j0][:, i1] * wx) * (1 - wy)[:, None] +
                (G[j1][:, i0] * (1 - wx) + G[j1][:, i1] * wx) * wy[:, None])

    ci = np.minimum((ts / (L / nbx)).astype(np.int64), nbx - 1)
    cj = np.minimum(((z1 - tz) / ((z1 - z0) / nby)).astype(np.int64), nby - 1)
    flat = (cj[:, None] * nbx + ci[None, :]).ravel()
    cols, means, tws = [], [], []
    for pid, vis, _, weight in cands:
        p = poses[pid]
        img = image(p['path'])
        H, W = img.shape[:2]
        az_rel, el = direction(p, X[None, :], Y[None, :], tz[:, None] + DATUM)
        px = (az_rel / 360 + 0.5) * W - 0.5
        py = (0.5 - el / 180) * H - 0.5
        col = sample(img, np.broadcast_to(px, el.shape), py)
        # (each texel's own tests - under the sky's edge, above the car - and its block's view, bilinear)
        sky = np.asarray(p['sky'])[(((az_rel + 180) / 360 * BINS).astype(np.int64)) % BINS]
        ok = (el > VEHICLE) & (el < sky - 0.2)
        tws.append(bilerp(vis) * ok * bilerp(weight))
        cols.append(col)
        s = np.stack([np.bincount(flat, weights=col[..., k].ravel(), minlength=nbx * nby) for k in range(3)], 1)
        cnt = np.bincount(flat, minlength=nbx * nby)
        means.append((s / np.maximum(cnt, 1)[:, None]).reshape(nby, nbx, 3))
    K = len(cands)
    # the exposures: each picture's matched to the main one's where both see the wall
    gains = [np.ones(3, np.float32) for _ in cands]
    for k in range(1, K):
        both = cands[0][1] & cands[k][1] & (means[k].min(-1) > 12) & (means[0].min(-1) > 12)
        if both.sum() >= 6:
            gains[k] = np.clip(np.median(means[0][both] / means[k][both], axis=0), 0.7, 1.45).astype(np.float32)
    # the main picture wherever it sees the wall, the others round it - but where three or more see a block and two or
    # more of the others agree on it and the main one does not, something stood in front of the wall in the main one
    # (a car, a person, a sign): the others' there
    V = np.stack([c[1] for c in cands])
    M = np.stack([means[k] * gains[k] for k in range(K)])
    pref = np.where(np.arange(K)[:, None, None] == 0, 1.0, 0.12) * np.ones((K, nby, nbx), np.float32)
    agreeing = 0.0
    if K >= 3:
        diff = np.abs(M[:, None] - M[None, :]).sum(-1)
        both = V[:, None] & V[None, :]
        cost = np.where(V, (diff * both).sum(1), np.inf)
        med = np.argmin(cost, 0)
        jj, ii = np.indices((nby, nbx))
        medM = M[med, jj, ii]
        support = (V & (np.abs(M - medM[None]).sum(-1) < 35)).sum(0)
        stood = V[0] & (V.sum(0) >= 3) & (np.abs(M[0] - medM).sum(-1) > 60) & (support >= 2) & (med != 0)
        for k in range(1, K):
            pref[k][stood & (med == k)] = 1.0
        pref[0][stood] = 0.0
        if V[0].any() and (V[0] & (V.sum(0) >= 2)).any():
            others = V[0] & (V[1:].any(0))
            agreeing = float((np.abs(M[0] - medM).sum(-1) < 60)[others].mean()) if others.any() else 0.0
    acc = np.zeros((TH, TW, 3), np.float32)
    wsum = np.zeros((TH, TW), np.float32)
    for k in range(K):
        wk = tws[k] * bilerp(pref[k])
        acc += cols[k] * gains[k] * wk[..., None]
        wsum += wk
    mask = wsum > 1e-4
    tex = np.where(mask[..., None], acc / np.maximum(wsum, 1e-9)[..., None], 0)
    # the checks (a wall seen far off through trees, or where the scan's building is not what stands there now, had come
    # out as leaves and shade):
    # (1) leaves and shade: green over red and blue with a busy texture, or dark - at most half of what the pictures saw
    seen_px = tex[mask]
    lum = tex.mean(-1)
    busy = np.zeros_like(lum)
    busy[1:-1, 1:-1] = np.abs(lum[1:-1, 2:] - lum[1:-1, :-2]) + np.abs(lum[2:, 1:-1] - lum[:-2, 1:-1])
    green = (tex[..., 1] > tex[..., 0] + 4) & (tex[..., 1] > tex[..., 2] + 4) & (busy > 12)
    leafy = float(((green | (lum < 38)) & mask).sum() / max(1, mask.sum()))
    # (2) its top against the sky where nothing on the scan stands over it behind: the main picture's skyline there within
    # 1.5 degrees of the top's elevation
    p0 = poses[cands[0][0]]
    cols = np.linspace(0.5, TW - 0.5, min(TW, 40)).astype(np.int64)
    qx, qy = X[cols], Y[cols]
    dist = np.hypot(qx - p0['fx'], qy - p0['fy'])
    az = np.degrees(np.arctan2(qx - p0['fx'], qy - p0['fy']))
    ztop = z1 - 0.5
    e_wall = np.degrees(np.arctan2(ztop - p0['zc'], dist))
    e_model = model_skyline(model_tops(d, p0['fx'], p0['fy'], az), p0['zc'])
    az_rel = (az - p0['fh'] + 180) % 360 - 180
    r = np.radians(az_rel)
    e_obs = e_wall + p0['tilt'][0] * np.cos(r) + p0['tilt'][1] * np.sin(r)
    sky = np.asarray(p0['sky'])[(((az_rel + 180) / 360 * BINS).astype(np.int64)) % BINS]
    facing0 = ((p0['fx'] - qx) * n[0] + (p0['fy'] - qy) * n[1]) > 0
    testable = facing0 & (np.abs(e_model - e_wall) < 0.7) & (e_wall > 1)
    skyline = float((np.abs(sky - e_obs) < 1.5)[testable].mean()) if testable.sum() >= 6 else -1.0
    # (3) else near and frontal, or two pictures agreeing
    v0 = cands[0][1]
    near_d = float(np.median(np.hypot(bx[None, :].repeat(nby, 0)[v0] - p0['fx'], by[None, :].repeat(nby, 0)[v0] - p0['fy']))) if v0.any() else 1e9
    # (4) no sky in the wall: the main picture's sky between a metre over the ground and a metre under the top (the place
    # or the building not as the scan has it)
    inner = (bz[:, None] > z0 + 1.6) & (bz[:, None] < ztop - 1.0) & np.ones((1, nbx), bool)
    skyward0 = cands[0][2] & inner
    facing_inner = inner & (cands[0][1] | cands[0][2])
    sky_in = float(skyward0.sum() / max(1, facing_inner.sum()))
    # (5) the wall where the scan has one: its building's returns a metre inside it, none 1.5 m out (a footprint off the
    # building that stands there - set back behind a lawn - had the picture of the lawn and the building behind on it)
    sx = np.linspace(0.05, 0.95, 24)
    px_, py_ = a[0] + u[0] * L * sx, a[1] + u[1] * L * sx
    ii, ij, iok = d.cells(px_ - n[0] * 1.0, py_ - n[1] * 1.0)
    oi, oj, ook = d.cells(px_ + n[0] * 1.5, py_ + n[1] * 1.5)
    inside_b = iok & ((d.flags[ij, ii] & 4) > 0)
    outside_b = ook & ((d.flags[oj, oi] & 4) > 0)
    placed = float((inside_b & ~outside_b).mean())
    # (6) mostly the pictures' own texels, not the fill (between half a metre over the ground and the top)
    rows = (tz > z0 + 1.1) & (tz < ztop)
    sampled = float(mask[rows].mean()) if rows.any() else 0.0
    ok = (leafy <= 0.5 and sky_in <= 0.1 and placed >= 0.6 and sampled >= 0.6 and
          (skyline >= 0.5 if skyline >= 0 else (near_d <= 30 or (K >= 2 and agreeing >= 0.6))))
    if not ok:
        return {'rejected': True, 'leafy': round(leafy, 3), 'skyline': round(skyline, 3), 'near': round(near_d, 1), 'agree': round(agreeing, 3)}
    tex, mask = storeys(tex, mask, rho)
    tex = push_pull(tex, mask)
    return {'width': TW, 'height': TH, 'density': rho, 'coverage': round(coverage, 3), 'agree': round(agreeing, 3), 'leafy': round(leafy, 3),
            'skyline': round(skyline, 3), 'near': round(near_d, 1), 'placed': round(placed, 3), 'sampled': round(sampled, 3), 'panos': [c[0] for c in cands],
            'pixels': np.clip(tex, 0, 255).astype(np.uint8).tobytes()}


def texture_chunk(job):
    out = []
    for w in job['walls']:
        r = texture_wall(w, job['poses'], job['density'])
        if r is not None:
            out.append((w, r))
    return out


# ---------------------------------------------------------------------------------------------------------- the ground

GROUND_GRID = 33     # the ground decal's heights: every 0.8 m (8 texels of 0.1 m) of a 25.6 m block
GROUND_REACH = 12.0  # how far from a picture its ground is used (m: farther, few texels and long smears of what stands on it)
GROUND_CAR = -19.0   # the ground under this elevation is the car's roof and hood (a pickup's reached -20 degrees: degrees)


def read_surface(path):
    """The build's surface.bin: the cell, a block's cells a side, and each block's (i, j, heights in m, rows south to north)."""
    with open(path, 'rb') as f:
        magic, version, cell, side, n = struct.unpack('<4sIfII', f.read(20))
        if magic != b'WBSF' or version != 1:
            sys.exit(f'{path}: not a surface.bin')
        blocks = []
        for _ in range(n):
            i, j = struct.unpack('<ii', f.read(8))
            blocks.append((i, j, np.frombuffer(f.read(2 * side * side), '<i2').reshape(side, side).astype(np.float32) / 100))
    return cell, side, blocks


def ground_block(job):
    """A block of the ground as the pictures show it: the decal's heights (the highest surface within 0.45 m of each
    vertex, 6 cm over it - the build lays the decal on those) and its picture: each texel from the nearest pictures that
    see it on those heights - within 16 m, outside the car, nothing on the scan's surface in the way - per 0.8 m cell the
    nearest's unless two others agree on something else (a car, a person, a shadow that moved); alpha: seen, fading out
    over a metre to where none sees it."""
    from scipy import ndimage
    d = _data
    i, j, h, cell, side = job['i'], job['j'], job['h'], job['cell'], job['side']
    step = side // (GROUND_GRID - 1)
    B = cell * side
    bx, by = i * B, j * B
    hm = ndimage.maximum_filter(h, size=9, mode='nearest')
    gidx = np.clip(np.arange(GROUND_GRID) * step, 0, side - 1)
    G = (hm[gidx][:, gidx] + 0.06).astype(np.float32)
    # (the texels' heights on the decal's triangles: a cell's (a, b, c) and (a, c, d), as the build draws them)
    t = (np.arange(side) + 0.5) / step
    ci = np.minimum(np.floor(t).astype(np.int64), GROUND_GRID - 2)
    f = t - ci
    fu, fv = f[None, :], f[:, None]
    a = G[ci[:, None], ci[None, :]]
    b_ = G[ci[:, None], ci[None, :] + 1]
    c_ = G[ci[:, None] + 1, ci[None, :] + 1]
    d_ = G[ci[:, None] + 1, ci[None, :]]
    Z = np.where(fu >= fv, a + (b_ - a) * fu + (c_ - b_) * fv, a + (c_ - d_) * fu + (d_ - a) * fv)
    X = (bx + (np.arange(side) + 0.5) * cell)[None, :] * np.ones((side, 1))
    Y = (by + (np.arange(side) + 0.5) * cell)[:, None] * np.ones((1, side))
    # (where something stood on the ground the scan saw - a car, a bin, a shrub, a sign's post: its picture smeared over the
    # ground there; the ground left as the build has it)
    si, sj, sok = d.cells(X, Y)
    stop = d.top[sj, si].astype(np.float32) / 100
    # (0.5-3 m over it: a crown over the street is no reason)
    standing = ndimage.binary_dilation(sok & (d.top[sj, si] > -32000) & (stop > Z + 0.5) & (stop < Z + 3.0), iterations=4)
    cols, wts, used = [], [], []
    for pid in job['cands']:
        p = job['poses'][pid]
        dx, dy = X - p['fx'], Y - p['fy']
        r = np.hypot(dx, dy)
        valid = (r <= GROUND_REACH) & (r >= 1.0)
        if not valid.any():
            continue
        az_rel, el = direction(p, X, Y, Z)
        valid &= (el > GROUND_CAR) & ~standing
        # (nothing on the scan's surface 0.4 m over the ray, up to 1.5 m short of the ground it meets)
        steps = np.arange(1.0, GROUND_REACH - 1.4, 0.5, dtype=np.float32)
        idx = np.nonzero(valid)
        rr = r[idx].astype(np.float32)
        ux, uy = dx[idx] / rr, dy[idx] / rr
        sx = p['fx'] + ux[:, None] * steps[None, :]
        sy = p['fy'] + uy[:, None] * steps[None, :]
        ii, jj, ok = d.cells(sx, sy)
        top = d.top[jj, ii].astype(np.float32) / 100
        ray = p['zc'] + (Z[idx] - p['zc'])[:, None] * steps[None, :] / rr[:, None]
        live = ok & (d.top[jj, ii] > -32000) & (steps[None, :] < rr[:, None] - 1.5)
        blocked = (live & (top > ray + 0.4)).any(1)
        v2 = np.zeros_like(valid)
        v2[idx[0][~blocked], idx[1][~blocked]] = True
        if v2.sum() < 50:
            continue
        img = image(p['path'])
        H, W = img.shape[:2]
        px = (az_rel / 360 + 0.5) * W - 0.5
        py = (0.5 - el / 180) * H - 0.5
        cols.append(sample(img, px, py))
        wts.append(v2 / np.maximum(r, 3) ** 2)
        used.append(pid)
    if not cols:
        return None
    K = len(cols)
    Wt = np.stack(wts)                                                  # (K, side, side)
    C = np.stack(cols)                                                  # (K, side, side, 3)
    n = side // step
    # (per 0.8 m cell: each picture's weight and mean colour where it sees)
    Wc = Wt.reshape(K, n, step, n, step).sum((2, 4))
    seen_c = (Wt > 0).reshape(K, n, step, n, step).mean((2, 4)) > 0.5
    Mc = (C * (Wt > 0)[..., None]).reshape(K, n, step, n, step, 3).sum((2, 4)) / np.maximum((Wt > 0).reshape(K, n, step, n, step).sum((2, 4)), 1)[..., None]
    best = np.argmax(Wc, 0)
    label = best.copy()
    if K >= 3:
        diff = np.abs(Mc[:, None] - Mc[None, :]).sum(-1)               # (K, K, n, n)
        both = seen_c[:, None] & seen_c[None, :]
        cost = np.where(seen_c, (diff * both).sum(1), np.inf)
        med = np.argmin(cost, 0)
        jj_, ii_ = np.indices((n, n))
        medM, bestM = Mc[med, jj_, ii_], Mc[best, jj_, ii_]
        support = (seen_c & (np.abs(Mc - medM[None]).sum(-1) < 35)).sum(0)
        swap = (seen_c.sum(0) >= 3) & (np.abs(bestM - medM).sum(-1) > 50) & (support >= 2) & (med != best)
        label = np.where(swap, med, best)
    # the texels: each picture by its share of the four nearest cells' labels, where it sees the texel
    fx = (np.arange(side) + 0.5) / step - 0.5
    fi = np.floor(fx).astype(np.int64)
    i0, i1 = np.clip(fi, 0, n - 1), np.clip(fi + 1, 0, n - 1)
    wx = (fx - fi).astype(np.float32)
    acc = np.zeros((side, side, 3), np.float32)
    wsum = np.zeros((side, side), np.float32)
    for k in range(K):
        L = (label == k).astype(np.float32)
        share = ((L[i0][:, i0] * (1 - wx) + L[i0][:, i1] * wx) * (1 - wx)[:, None] + (L[i1][:, i0] * (1 - wx) + L[i1][:, i1] * wx) * wx[:, None])
        share = share * (Wt[k] > 0)
        acc += C[k] * share[..., None]
        wsum += share
    seen = wsum > 1e-3
    # (where the cells' pictures do not see a texel another one does: the nearest that does)
    fill = ~seen & (Wt.max(0) > 0)
    if fill.any():
        k = np.argmax(Wt, 0)
        acc[fill] = C[k[fill], np.nonzero(fill)[0], np.nonzero(fill)[1]]
        wsum[fill] = 1
        seen |= fill
    if seen.sum() < 0.02 * seen.size:
        return None
    rgb = np.where(seen[..., None], acc / np.maximum(wsum, 1e-6)[..., None], 0)
    # the paint alone: the whole picture laid on the ground had read as a smeared decal - cars, the camera's hood, a
    # hydrant flat on the road, its colours off the road's round it, a seam at its edge. White and yellow marks well over
    # the asphalt round them (its median over 1.5 m), thin - lines, bars, letters; a white car's roof, a wall, a sign's face
    # over 0.9 m across is no mark -, in clean paint colours; elsewhere the picture is seen (alpha 1: the build hides its
    # own markings there - the picture's are the street's) or not (0)
    lum = rgb.mean(-1)
    bg = ndimage.median_filter(np.where(seen, lum, lum[seen].mean()), size=15)
    mx, mn = rgb.max(-1), rgb.min(-1)
    sat = (mx - mn) / np.maximum(mx, 1)
    r_, g_, b_ = rgb[..., 0], rgb[..., 1], rgb[..., 2]
    white = (lum > bg + 40) & (sat < 0.22) & (lum > 120)
    yellow = (r_ > 130) & (g_ > 105) & (b_ < 0.72 * g_) & (r_ - b_ > 50) & (lum > bg + 18)
    paint = ndimage.binary_opening((white | yellow) & seen)
    lab, n = ndimage.label(paint)
    if n:
        idx = np.arange(1, n + 1)
        widest = ndimage.maximum(ndimage.distance_transform_edt(paint), lab, idx)
        size = ndimage.sum(paint, lab, idx)
        keep = np.zeros(n + 1, bool)
        keep[1:] = (np.asarray(widest) <= 4.5) & (np.asarray(size) >= 6)
        paint = keep[lab]
    soft = np.clip(ndimage.gaussian_filter(paint.astype(np.float32), 0.7) * 1.6, 0, 1)
    colour = np.where(yellow[..., None], np.array([212, 172, 46], np.float32), np.array([228, 228, 222], np.float32))
    alpha = np.where(soft > 0.03, np.maximum(soft * 255, 9), np.where(seen, 1, 0))
    rgba = np.concatenate([colour, alpha[..., None]], -1).astype(np.uint8)
    return {'i': i, 'j': j, 'grid': G.astype('<f4').tobytes(), 'pixels': rgba[::-1].tobytes(), 'seen': float(seen.mean()), 'panos': used,
            'paint': float(paint.mean())}


def ground_step(out, fitted, workers, cache):
    """phototex.py --ground: the build's surface.bin (worldgen build --phototex: the ground's top round the pictures) to
    ground.json and ground.bin - each block: its decal's heights (33 x 33 f32, rows south to north), then its picture (256 x
    256 RGBA, rows from the north) - for the next build's decals."""
    path = os.path.join(out, 'surface.bin')
    if not os.path.exists(path):
        print('no surface.bin yet: build once with --phototex (it writes it), then run --ground again', flush=True)
        return
    cell, side, blocks = read_surface(path)
    pts = np.array([[p['fx'], p['fy']] for p in fitted.values()])
    ids = list(fitted)
    B = cell * side
    jobs = []
    for i, j, h in blocks:
        cx, cy = (i + 0.5) * B, (j + 0.5) * B
        near = np.nonzero(np.hypot(pts[:, 0] - cx, pts[:, 1] - cy) < GROUND_REACH + B * 0.75)[0]
        if not len(near):
            continue
        near = near[np.argsort(np.hypot(pts[near, 0] - cx, pts[near, 1] - cy))][:8]
        cands = [ids[k] for k in near]
        jobs.append({'i': i, 'j': j, 'h': h, 'cell': cell, 'side': side, 'cands': cands, 'poses': {c: fitted[c] for c in cands}})
    listed, offset, t0 = [], 0, time.time()
    with open(os.path.join(out, 'ground.bin'), 'wb') as fb, Pool(workers, worker_init, (os.path.join(out, 'cache'),)) as pool:
        for k, r in enumerate(pool.imap_unordered(ground_block, jobs, chunksize=2)):
            if r is not None:
                fb.write(r['grid'])
                fb.write(r['pixels'])
                listed.append({'i': r['i'], 'j': r['j'], 'offset': offset, 'seen': round(r['seen'], 3)})
                offset += len(r['grid']) + len(r['pixels'])
            if k % 100 == 0:
                print(f'  ground: {k + 1}/{len(jobs)} blocks, {len(listed)} pictured ({time.time() - t0:.0f} s)', flush=True)
    with open(os.path.join(out, 'ground.json'), 'w', encoding='utf-8') as f:
        json.dump({'cell': cell, 'side': side, 'grid': GROUND_GRID, 'blocks': listed}, f)
    print(f'the ground: {len(listed)} blocks of {len(jobs)} pictured ({offset / 1e6:.0f} MB)', flush=True)


# ----------------------------------------------------------------------------------------------------------- the run

def roof_top(d, fp):
    """A footprint's highest building return (m), None if the scan has none in it."""
    p = np.array(fp, np.float64)
    x0, y0 = p.min(0)
    x1, y1 = p.max(0)
    xs = np.arange(math.floor(x0 / CELL) * CELL + CELL / 2, x1, CELL)
    ys = np.arange(math.floor(y0 / CELL) * CELL + CELL / 2, y1, CELL)
    if not len(xs) or not len(ys):
        return None
    X, Y = np.meshgrid(xs, ys)
    inside = np.zeros(X.shape, bool)
    for k in range(len(p)):
        (ax, ay), (bx, by) = p[k], p[(k + 1) % len(p)]
        cross = ((ay > Y) != (by > Y)) & (X < (bx - ax) * (Y - ay) / np.where(by - ay == 0, 1e-9, by - ay) + ax)
        inside ^= cross
    if not inside.any():
        return None
    i, j, ok = d.cells(X[inside], Y[inside])
    r = d.roof[j, i]
    r = r[ok & (r > -32000)]
    return float(r.max()) / 100 if len(r) else None


def main():
    o = options(sys.argv[1:])
    for k in ('world', 'metadata', 'scan', 'ground', 'out'):
        if k not in o:
            sys.exit(__doc__)
    out = o['out']
    os.makedirs(out, exist_ok=True)
    workers = int(o.get('workers', 12))
    density = float(o.get('density', 16))
    miss_max = float(o.get('miss', 6.0))
    agree_min = float(o.get('agree', 0.35))
    with open(o['world'], encoding='utf-8') as f:
        world = json.load(f)
    pr = world['projection']
    game = lambda lat, lon: (pr['x0'] + (lon - pr['lon0']) * pr['kx'], pr['y0'] + (lat - pr['lat0']) * pr['ky'])
    rows = []
    for meta in o['metadata'].split(','):
        with open(meta, encoding='utf-8') as f:
            for r in csv.DictReader(f):
                path = os.path.join(os.path.dirname(meta), r['file'])
                if r.get('pano') == 'True' and os.path.exists(path):
                    x, y = game(float(r['lat']), float(r['lon']))
                    rows.append({'id': r['id'], 'path': path, 'x': x, 'y': y, 'heading': float(r['heading']), 'date': r['date'],
                                 'creator': r['creator']})
    if 'area' in o:
        ax0, ay0, ax1, ay1 = (float(v) for v in o['area'].split(','))
        rows = [r for r in rows if ax0 <= r['x'] <= ax1 and ay0 <= r['y'] <= ay1]
    if 'ids' in o:
        wanted = set(o['ids'].split(','))
        rows = [r for r in rows if r['id'] in wanted]
    if not rows:
        sys.exit('no panoramas there')
    xs, ys = [r['x'] for r in rows], [r['y'] for r in rows]
    box = (math.floor(min(xs) - 450), math.floor(min(ys) - 450), math.ceil(max(xs) + 450), math.ceil(max(ys) + 450))
    cache = os.path.join(out, 'cache')
    t0 = time.time()
    make_cache(o['scan'], o['ground'], box, cache, world)
    print(f'{len(rows)} panoramas; the scan over {box} ({time.time() - t0:.0f} s)', flush=True)
    worker_init(cache)
    # 1. the fits (kept)
    pose_path = os.path.join(out, 'poses.json')
    poses = {}
    if os.path.exists(pose_path) and o.get('redo') != '1':
        with open(pose_path, encoding='utf-8') as f:
            poses = {p['id']: p for p in json.load(f)}
    todo = [r for r in rows if r['id'] not in poses]
    if todo:
        t0 = time.time()
        with Pool(workers, worker_init, (cache,)) as pool:
            for k, p in enumerate(pool.imap_unordered(register, todo, chunksize=1)):
                poses[p['id']] = p
                if k % 20 == 0:
                    print(f'  fit {k + 1}/{len(todo)} ({time.time() - t0:.0f} s): {p["id"]} ' +
                          (f'moved {p["moved"]} m, turned {p["turned"]}, {p["above"]} m up, miss {p["miss"]}, agree {p["agree"]}' if p['ok'] else p['why']),
                          flush=True)
        with open(pose_path, 'w', encoding='utf-8') as f:
            json.dump(list(poses.values()), f)
    wanted = {r['id'] for r in rows}
    fitted = {k: p for k, p in poses.items() if p['ok'] and p['miss'] <= miss_max and p['agree'] >= agree_min and k in wanted}
    misses = sorted(p['miss'] for p in poses.values() if p['ok'])
    print(f'fits: {len(fitted)} of {len(rows)} used (miss <= {miss_max}, agree >= {agree_min}); misses at the quartiles ' +
          ', '.join(f'{misses[int(q * (len(misses) - 1))]:.2f}' for q in (0.25, 0.5, 0.75)) if misses else 'no fits', flush=True)
    n_check = int(o.get('check', 0))
    for p in sorted(poses.values(), key=lambda p: p['id'])[:n_check]:
        if p['ok']:
            check_picture(p, cache, out)
    # 2. the walls the pictures face
    d = _data
    walls = []
    pts = np.array([[p['fx'], p['fy']] for p in fitted.values()]) if fitted else np.zeros((0, 2))
    ids = list(fitted)
    for bi, bld in enumerate(world['buildings']):
        fp = bld['footprint']
        if len(fp) < 3 or not len(pts):
            continue
        c = np.mean(np.array(fp), 0)
        span = float(np.max(np.hypot(*(np.array(fp) - c).T)))
        near = np.hypot(pts[:, 0] - c[0], pts[:, 1] - c[1]) < REACH + span
        if not near.any():
            continue
        top = roof_top(d, fp)
        gnds = d.ground(np.array([q[0] for q in fp]), np.array([q[1] for q in fp]))
        if np.isnan(gnds).all():
            continue
        base = float(np.nanmin(gnds))
        z1 = (top if top is not None and top > base + 2 else base + max(3.0, float(bld.get('height') or 6))) + 0.5
        z1 = min(z1, base + 120)
        for e in range(len(fp)):
            a, b = np.array(fp[e], np.float64), np.array(fp[(e + 1) % len(fp)], np.float64)
            L = float(np.hypot(*(b - a)))
            if L < 1.0:
                continue
            u = (b - a) / L
            nrm = np.array([u[1], -u[0]])
            mid = (a + b) / 2
            g = d.ground(np.array([a[0], mid[0], b[0]]), np.array([a[1], mid[1], b[1]]))
            z0 = float(np.nanmin(g)) - 0.6 if not np.isnan(g).all() else base - 0.6
            # (the pictures in front of it, near: the nearest first)
            rel = pts - a
            along = np.clip(rel @ u, 0, L)
            dist = np.hypot(pts[:, 0] - (a[0] + u[0] * along), pts[:, 1] - (a[1] + u[1] * along))
            front = (rel @ nrm) > 0.5
            k = np.nonzero(front & (dist < REACH))[0]
            if not len(k):
                continue
            k = k[np.argsort(dist[k])][:12]
            walls.append({'building': bi, 'edge': e, 'a': [round(float(a[0]), 3), round(float(a[1]), 3)],
                          'b': [round(float(b[0]), 3), round(float(b[1]), 3)], 'z0': round(z0, 2), 'z1': round(z1, 2),
                          'cands': [ids[i] for i in k]})
    print(f'{len(walls)} walls face a picture within {REACH:.0f} m', flush=True)
    # 3. their textures, in chunks of 150 m (the pictures of a chunk kept in its worker)
    chunks = {}
    for w in walls:
        key = (int((w['a'][0] + w['b'][0]) / 2 // 150), int((w['a'][1] + w['b'][1]) / 2 // 150))
        chunks.setdefault(key, []).append(w)
    jobs = []
    for key, ws in sorted(chunks.items()):
        for s in range(0, len(ws), 40):
            part = ws[s:s + 40]
            need = {c for w in part for c in w['cands']}
            jobs.append({'walls': part, 'poses': {c: fitted[c] for c in need}, 'density': density})
    listed = []
    offset = 0
    used_panos = set()
    t0 = time.time()
    with open(os.path.join(out, 'walls.bin'), 'wb') as fb, Pool(workers, worker_init, (cache,)) as pool:
        rejected = 0
        for k, res in enumerate(pool.imap_unordered(texture_chunk, jobs, chunksize=1)):
            for w, r in res:
                if r.get('rejected'):
                    rejected += 1
                    continue
                fb.write(r['pixels'])
                listed.append({'building': w['building'], 'edge': w['edge'], 'a': w['a'], 'b': w['b'], 'z0': w['z0'], 'z1': w['z1'],
                               'width': r['width'], 'height': r['height'], 'offset': offset, 'density': r['density'],
                               'coverage': r['coverage'], 'agree': r['agree'], 'leafy': r['leafy'], 'skyline': r['skyline'], 'near': r['near'],
                               'panos': r['panos']})
                offset += len(r['pixels'])
                used_panos.update(r['panos'])
            if k % 10 == 0:
                print(f'  walls: {k + 1}/{len(jobs)} chunks, {len(listed)} textures, {offset / 1e6:.0f} MB ({time.time() - t0:.0f} s)', flush=True)
    pictures = [{kk: p[kk] for kk in ('id', 'x', 'y', 'heading', 'fx', 'fy', 'zc', 'fh', 'tilt', 'miss', 'agree', 'date', 'creator')}
                for p in fitted.values() if p['id'] in used_panos]
    creators = sorted({p['creator'] for p in pictures})
    with open(os.path.join(out, 'phototex.json'), 'w', encoding='utf-8') as f:
        json.dump({'licence': 'Mapillary, CC BY-SA 4.0', 'creators': creators, 'pictures': pictures, 'walls': listed}, f)
    print(f'{len(listed)} wall textures ({offset / 1e6:.0f} MB) from {len(used_panos)} pictures by {", ".join(creators)}; {rejected} failed the checks',
          flush=True)
    # (a look at some)
    if n_check and listed:
        with open(os.path.join(out, 'walls.bin'), 'rb') as fb:
            big = sorted(listed, key=lambda w: -w['width'] * w['height'] * w['coverage'])[:n_check * 3]
            for k in range(0, len(big), 6):
                ims = []
                for w in big[k:k + 6]:
                    fb.seek(w['offset'])
                    im = Image.frombytes('RGB', (w['width'], w['height']), fb.read(w['width'] * w['height'] * 3))
                    im.thumbnail((600, 300))
                    ims.append(im)
                sheet = Image.new('RGB', (1200, 300 * ((len(ims) + 1) // 2)), (255, 255, 255))
                for q, im in enumerate(ims):
                    sheet.paste(im, ((q % 2) * 600, (q // 2) * 300))
                sheet.save(os.path.join(out, f'walls_{k // 6}.jpg'), quality=88)
    # 4. the ground (--ground 1: from the build's surface.bin)
    if o.get('ground') == '1':
        ground_step(out, fitted, workers, cache)


if __name__ == '__main__':
    main()
