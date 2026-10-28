"""What the aerial pictures show of a sector (research/phase0.md §63), for worldgen build --photo: every tree that
stands there and the ground's cover and colour, from the USDA's NAIP (public domain; over Florida 2023, 0.3 m: red, green,
blue, near infrared) through The National Map, read at 1 m onto the sector's game coordinates, with Meta and WRI's
canopy heights (CC BY 4.0: "High Resolution Canopy Height Maps", 1 m from satellite pictures of about 2018-2020, tiles
in EPSG:3857) for how tall the trees are.

usage (the venv in D:\\GTAV-UE5\\venv: numpy, scipy, tifffile, zarr, Pillow):
  photo.py --world <world.json> --out <dir> [--area x0,y0,x1,y1] [--chm D:\\GTAV-UE5\\chm] [--keep <dir>] [--fetch 4]
  <dir>/trees.bin  'WBTR' version 1, count (u32); each tree: x, y, height, crown radius (f32), kind (u8: 0 evergreen, 1 bare
                   - leafless in the pictures' winter, 2 palm-like: small round crown), pad (3 bytes)
  <dir>/cover.bin  'WBPC' version 1, x0, y0, cell (f32), nx, ny (u32); each 4 m cell (rows south to north): its class (u8:
                   0 not seen, 1 water, 2 tree canopy, 3 green grass, 4 dry grass, 5 bare earth or sand, 6 paving or roof),
                   red, green, blue (u8: the mean colour of the cell's ground the pictures see, not the trees')
  <dir>/classes.bin 'WBCL' version 1, x0, y0, cell (f32: 1 m), nx, ny (u32); each metre's class as above (rows south to
                   north; over --area, else the world's box)
  --keep: the 1 m pictures (naip_<i>_<j>.npz) kept there, read from there again
The pictures are asked for in blocks of 2 km (4 bands, JPEG); a tree is a peak of the canopy's brightness in the near
infrared (its sunlit crown) - the canopy where the canopy heights say 3 m and more or the near infrared is high and
uneven (crowns and their shadows, not a lawn's even green) - at least a crown's reach from a taller one.
"""
import io, json, math, os, struct, sys, time, urllib.parse, urllib.request

import numpy as np
from PIL import Image, ImageDraw
from scipy import ndimage

NAIP = 'https://imagery.nationalmap.gov/arcgis/rest/services/USGSNAIPPlus/ImageServer/exportImage'
CHM_URL = 'https://dataforgood-fb-data.s3.amazonaws.com/forests/v1/alsgedi_global_v6_float/chm/{}.tif'
AGENT = {'User-Agent': 'GTAVE-ModLoader-worldgen/1.0'}
BLOCK, OVERLAP, CELL, GROUND = 2000, 32, 1.0, 4.0
R_EARTH = 6378137.0


def options(argv):
    o = {'chm': r'D:\GTAV-UE5\chm'}
    for i in range(0, len(argv) - 1, 2):
        if not argv[i].startswith('--'):
            sys.exit(__doc__)
        o[argv[i][2:]] = argv[i + 1]
    if 'world' not in o or 'out' not in o:
        sys.exit(__doc__)
    return o


def fetch(url, tries=9):
    # (the picture service goes down for minutes at a time - 504s for a quarter of an hour took four lands of the
    # statewide batch: waits of 15 s doubling to 5 min, about 20 min in all)
    for k in range(tries):
        try:
            with urllib.request.urlopen(urllib.request.Request(url, headers=AGENT), timeout=180) as r:
                return r.read()
        except Exception:
            if k == tries - 1:
                raise
            time.sleep(min(300, 15 * 2 ** k))


class Projection:
    def __init__(self, p):
        self.lat0, self.lon0, self.x0, self.y0, self.kx, self.ky = p['lat0'], p['lon0'], p['x0'], p['y0'], p['kx'], p['ky']

    def latlon(self, x, y):
        return self.lat0 + (y - self.y0) / self.ky, self.lon0 + (x - self.x0) / self.kx


def naip_block(pr, x0, y0, nx, ny):
    """RGBN of the game box [x0, x0 + nx) x [y0, y0 + ny) at 1 m, rows north to south (as the picture's)."""
    s, w = pr.latlon(x0, y0)
    n, e = pr.latlon(x0 + nx * CELL, y0 + ny * CELL)
    out = np.zeros((ny, nx, 4), np.uint8)
    for bands, at in (('0,1,2', slice(0, 3)), ('3', slice(3, 4))):
        q = urllib.parse.urlencode({'bbox': f'{w},{s},{e},{n}', 'bboxSR': 4326, 'imageSR': 4326, 'size': f'{nx},{ny}', 'bandIds': bands,
                                    'format': 'jpg', 'compressionQuality': 92, 'f': 'image'})
        im = Image.open(io.BytesIO(fetch(f'{NAIP}?{q}')))
        a = np.asarray(im.convert('RGB' if bands != '3' else 'L'))
        if a.shape[:2] != (ny, nx):
            a = np.asarray(Image.fromarray(a).resize((nx, ny), Image.BILINEAR))
        out[..., at] = a.reshape(ny, nx, -1)
    return out


class Canopy:
    """Meta and WRI's canopy heights (m, uint8) by latitude and longitude: their zoom-9 Web Mercator tiles, downloaded
    once into the folder."""

    def __init__(self, folder):
        self.folder = folder
        self.tiles = {}

    @staticmethod
    def quadkey(tx, ty, z=9):
        k = ''
        for i in range(z, 0, -1):
            d = 0
            m = 1 << (i - 1)
            if tx & m:
                d += 1
            if ty & m:
                d += 2
            k += str(d)
        return k

    def tile(self, key):
        if key not in self.tiles:
            import tifffile, zarr
            path = os.path.join(self.folder, key + '.tif')
            if not os.path.exists(path):
                os.makedirs(self.folder, exist_ok=True)
                print(f'  canopy tile {key}: downloading', flush=True)
                with urllib.request.urlopen(urllib.request.Request(CHM_URL.format(key), headers=AGENT), timeout=600) as r, open(path + '.part', 'wb') as f:
                    while True:
                        b = r.read(1 << 22)
                        if not b:
                            break
                        f.write(b)
                os.replace(path + '.part', path)
            with tifffile.TiffFile(path) as t:
                p = t.pages[0]
                scale = p.tags['ModelPixelScaleTag'].value
                tie = p.tags['ModelTiepointTag'].value
            self.tiles[key] = (zarr.open(tifffile.imread(path, aszarr=True), mode='r'), tie[3], tie[4], scale[0])
        return self.tiles[key]

    def heights(self, lat, lon):
        """Heights at arrays of latitude and longitude (nearest pixel)."""
        mx = R_EARTH * np.radians(lon)
        my = R_EARTH * np.log(np.tan(np.pi / 4 + np.radians(lat) / 2))
        world = 2 * math.pi * R_EARTH
        tx = np.floor((mx + world / 2) / world * 512).astype(np.int64)
        ty = np.floor((world / 2 - my) / world * 512).astype(np.int64)
        out = np.zeros(lat.shape, np.uint8)
        for key_xy in set(zip(tx.ravel().tolist(), ty.ravel().tolist())):
            sel = (tx == key_xy[0]) & (ty == key_xy[1])
            z, ox, oy, px = self.tile(self.quadkey(*key_xy))
            col = np.floor((mx[sel] - ox) / px).astype(np.int64)
            row = np.floor((oy - my[sel]) / px).astype(np.int64)
            r0, r1, c0, c1 = row.min(), row.max() + 1, col.min(), col.max() + 1
            window = np.asarray(z[max(r0, 0):r1, max(c0, 0):c1])
            rr, cc = np.clip(row - max(r0, 0), 0, window.shape[0] - 1), np.clip(col - max(c0, 0), 0, window.shape[1] - 1)
            out[sel] = window[rr, cc]
        return out


def analyse(rgbn, chm, near_built=None):
    """One block (rows north to south): the canopy, its trees (block pixel coordinates), and each pixel's ground class.
    `near_built`: the pixels within 6 m of a building (the map's and the footprints'), or None."""
    r, g, b, n = (rgbn[..., k].astype(np.float32) for k in range(4))
    ndvi = (n - r) / np.maximum(n + r, 1)
    bright = (r + g + b) / 3
    # (unevenness of the near infrared over 5 m: crowns and their shadows; a lawn or a field is even)
    nmean = ndimage.uniform_filter(n, 5)
    nvar = ndimage.uniform_filter(n * n, 5) - nmean * nmean
    uneven = np.sqrt(np.maximum(nvar, 0))
    # (water takes in the near infrared: dark there against the visible light - the bay's turquoise shallows as well as a
    # black pond)
    water = ((ndvi < 0.02) & (bright < 60) & (n < 55)) | ((ndvi < -0.05) & (n < 0.75 * bright))
    # (shade - of the trees, of the buildings - is as dark and its near infrared as low: the water is what lies in smooth
    # stretches of 300 m² and more with no canopy heights over it, the rest shade; in Tallahassee's yards a seventh of the
    # ground had been water, its ground brown leaf litter)
    shade = water.copy()
    labels, count = ndimage.label(ndimage.binary_opening(water, iterations=1))
    if count:
        index = np.arange(1, count + 1)
        size = ndimage.sum(np.ones(labels.shape, np.float32), labels, index)
        rough = ndimage.mean(uneven, labels, index)
        tall = ndimage.mean((chm >= 3).astype(np.float32), labels, index)
        keep = np.zeros(count + 1, bool)
        keep[1:] = (size >= 300) & (rough < 12) & (tall < 0.3)
        water = ndimage.binary_dilation(keep[labels], iterations=1) & water
    else:
        water = np.zeros_like(water)
    shade &= ~water
    # (the pictures decide where trees stand - crowns and their shadows make the near infrared uneven, 17-26 over 5 m in
    # woods, about 5 on a field; the canopy heights are years older: a field cleared since still had 15 m trees there -
    # and only give their heights; leafless winter crowns: uneven, a little green or tall)
    # (little green - a winter's leafless crowns, or a field's shrubs and tracks: only more uneven, or tall)
    canopy = (uneven > 12) & ((ndvi > 0.15) | (chm >= 3) | ((ndvi > 0.08) & (uneven > 17))) | ((ndvi > 0.35) & (uneven > 8))
    canopy &= ~water
    # (crowns in their own shade, and the shade they throw under the canopy heights' trees: canopy - what is seen from
    # above; the crowns had covered a third less than the pictures' canopy)
    canopy |= shade & (chm >= 4)
    # (never bright bare ground - a sand track, a ploughed field - between crowns: the 5 m unevenness spreads onto it)
    canopy &= ~((bright > 110) & (ndvi < 0.1))
    canopy = ndimage.binary_opening(canopy, iterations=1)
    canopy = ndimage.binary_closing(canopy, iterations=2)
    # (by a building only canopy the canopy heights see - 4 m and more - and green: a lawn's edge against a building's
    # shadow, or the roof's edge, had been uneven enough to be canopy - the reviewers' row of identical trees along every
    # garage's and office's facade)
    if near_built is not None:
        canopy &= ~near_built | ((chm >= 4) & (ndvi > 0.2))
    # (a crown on its own - a canopy patch about as round as one crown, no wider than a live oak's for its height - one
    # tree at its middle: the peaks had cut a lawn's live oak into a grove of small trees, the reviewers' "three trunks
    # where the photo has one oak")
    single = np.zeros(canopy.shape, bool)
    alone = []
    labels, count = ndimage.label(canopy)
    for k, sl in enumerate(ndimage.find_objects(labels)):
        if sl is None:
            continue
        part = labels[sl] == k + 1
        area = float(part.sum())
        if area < 12 or area > 1200:
            continue
        yy, xx = np.nonzero(part)
        cy, cx = float(yy.mean()), float(xx.mean())
        rmax = float(np.sqrt(((yy - cy) ** 2 + (xx - cx) ** 2).max())) + 0.5
        h = float(chm[sl][part].max())
        if area / (math.pi * rmax * rmax) < 0.55 or (h >= 3 and math.sqrt(area / math.pi) > max(4.0, h)):
            continue
        single[sl] |= part
        alone.append((int(round(cy)) + sl[0].start, int(round(cx)) + sl[1].start, h))
    # trees: peaks of the smoothed near infrared (sunlit crowns) in the canopy, at least a reach apart
    signal = ndimage.gaussian_filter(n + 0.6 * np.maximum(chm.astype(np.float32) - 3, 0) * 6, 1.6)
    signal[~canopy] = 0
    peaks = (signal == ndimage.maximum_filter(signal, size=7)) & canopy & (signal > 0) & ~single
    ys, xs = np.nonzero(peaks)
    order = np.argsort(-signal[ys, xs])
    ys, xs = ys[order], xs[order]
    # canopy heights round each peak: the highest within 3 m (the crown's top); none (young or new growth) -> from the
    # canopy round it
    chm_max = ndimage.maximum_filter(chm, size=7).astype(np.float32)
    chm_wide = ndimage.uniform_filter(np.where(chm >= 3, chm, 0).astype(np.float32), 31)
    chm_cnt = ndimage.uniform_filter((chm >= 3).astype(np.float32), 31)
    taken = np.zeros(canopy.shape, bool)
    trees = []
    dist_out = ndimage.distance_transform_edt(canopy)  # (how deep in the canopy: a new wood's trees' height)
    for y, x in zip(ys.tolist(), xs.tolist()):
        if taken[y, x]:
            continue
        h = float(chm_max[y, x])
        if h < 3:
            h = float(chm_wide[y, x] / chm_cnt[y, x]) if chm_cnt[y, x] > 0.05 else 0.0
            if h < 3:
                h = 9.0 + 4.0 * min(1.0, float(dist_out[y, x]) / 15)
        reach = int(round(min(6.0, max(2.5, 1.2 + 0.16 * h))))
        y0, y1, x0, x1 = max(0, y - reach), min(canopy.shape[0], y + reach + 1), max(0, x - reach), min(canopy.shape[1], x + reach + 1)
        yy, xx = np.ogrid[y0:y1, x0:x1]
        disc = (yy - y) ** 2 + (xx - x) ** 2 <= reach * reach
        taken[y0:y1, x0:x1] |= disc
        evergreen = ndvi[y, x] > 0.18
        kind = 0 if evergreen else 1
        trees.append([x, y, h, 0.0, kind])
    for y, x, h in alone:
        if h < 3:
            h = float(chm_wide[y, x] / chm_cnt[y, x]) if chm_cnt[y, x] > 0.05 else 0.0
            if h < 3:
                h = 9.0 + 4.0 * min(1.0, float(dist_out[y, x]) / 15)
        trees.append([x, y, h, 0.0, 0 if ndvi[y, x] > 0.18 else 1])
    # each tree's crown: the canopy it covers - every canopy pixel to its nearest tree, the crown a circle of that area
    # (the game's trees are fewer than a plantation's: their crowns fill the canopy the pictures show, not more)
    if trees:
        seeds = np.zeros(canopy.shape, bool)
        index = np.zeros(canopy.shape, np.int32)
        for k, t in enumerate(trees):
            seeds[t[1], t[0]] = True
            index[t[1], t[0]] = k
        _, (iy, ix) = ndimage.distance_transform_edt(~seeds, return_indices=True)
        owner = index[iy, ix]
        area = np.bincount(owner[canopy], minlength=len(trees))
        for k, t in enumerate(trees):
            t[3] = float(min(15.0, max(1.5, math.sqrt(area[k] / math.pi))))  # (a live oak's: up to 15 m)
    trees = [tuple(t) for t in trees]
    # the ground under no canopy: water, green grass, dry grass, bare earth or sand, paving or roofs
    sat = (np.maximum(np.maximum(r, g), b) - np.minimum(np.minimum(r, g), b)) / np.maximum(np.maximum(np.maximum(r, g), b), 1)
    # (no plants: grey - paving, roofs - at any brightness, a city's light concrete is no sand; warm and bright - sand,
    # bare earth, a ploughed field; warm and darker the winter's dormant grass, dry grass)
    cls = np.full(canopy.shape, 4, np.uint8)
    cls[(ndvi > 0.28)] = 3
    warm = (r > b + 14) | (sat >= 0.16)
    cls[(ndvi <= 0.12) & warm & (bright > 150)] = 5
    cls[(ndvi <= 0.12) & ~warm] = 6
    cls[water] = 1
    cls[canopy] = 2
    # (other shade: the ground under it as the nearest ground in the open - a lawn's shade lawn, a car park's paving)
    rest = shade & ~canopy
    if rest.any() and not (rest | canopy | water).all():
        _, (iy, ix) = ndimage.distance_transform_edt(rest | canopy | water, return_indices=True)
        cls[rest] = cls[iy[rest], ix[rest]]
    return canopy, trees, cls, shade


def main():
    o = options(sys.argv[1:])
    with open(o['world'], encoding='utf-8') as f:
        w = json.load(f)
    pr = Projection(w['projection'])
    if 'area' in o:
        ax0, ay0, ax1, ay1 = (float(v) for v in o['area'].split(','))
    else:
        ax0, ay0, ax1, ay1 = w['min'][0], w['min'][1], w['max'][0], w['max'][1]
    out = o['out']
    os.makedirs(out, exist_ok=True)
    chm = Canopy(o['chm'])
    # the buildings' footprints by 500 m cell (the map's and the footprints merged into the world)
    by_cell = {}
    for b in w.get('buildings', []):
        fp = b.get('footprint') or []
        if len(fp) < 3:
            continue
        xs_, ys_ = [p[0] for p in fp], [p[1] for p in fp]
        for i in range(int(math.floor(min(xs_) / 500)), int(math.floor(max(xs_) / 500)) + 1):
            for j in range(int(math.floor(min(ys_) / 500)), int(math.floor(max(ys_) / 500)) + 1):
                by_cell.setdefault((i, j), []).append(fp)

    def footprints_near(bx0, by0, bx1, by1):
        seen = set()
        for i in range(int(math.floor(bx0 / 500)), int(math.floor(bx1 / 500)) + 1):
            for j in range(int(math.floor(by0 / 500)), int(math.floor(by1 / 500)) + 1):
                for fp in by_cell.get((i, j), []):
                    if id(fp) not in seen:
                        seen.add(id(fp))
                        yield fp
    # the ground grid: the land's 4 m cells (worldgen's: from the world's corner)
    gx0, gy0 = w['min'][0], w['min'][1]
    gnx, gny = int(math.ceil((w['max'][0] - gx0) / GROUND)), int(math.ceil((w['max'][1] - gy0) / GROUND))
    gsum = np.zeros((gny, gnx, 3), np.float64)
    gcount = np.zeros((gny, gnx), np.float64)
    gclass = np.zeros((gny, gnx, 7), np.float32)
    # the 1 m classes (rows south to north)
    fnx, fny = int(math.ceil((ax1 - ax0) / CELL)), int(math.ceil((ay1 - ay0) / CELL))
    fine = np.zeros((fny, fnx), np.uint8)
    trees = []
    started = time.time()
    blocks = [(bx, by) for by in np.arange(ay0, ay1, BLOCK) for bx in np.arange(ax0, ax1, BLOCK)]
    def window(bx, by):
        # the block and its overlap (its trees kept only in the block itself)
        x0, y0 = bx - OVERLAP, by - OVERLAP
        return x0, y0, int(min(BLOCK, ax1 - bx) + 2 * OVERLAP), int(min(BLOCK, ay1 - by) + 2 * OVERLAP)

    def picture(bx, by):
        x0, y0, nx, ny = window(bx, by)
        cached = os.path.join(o['keep'], f'naip_{int(bx)}_{int(by)}.npz') if 'keep' in o else None
        if cached and os.path.exists(cached):
            return np.load(cached)['rgbn']
        rgbn = naip_block(pr, x0, y0, nx, ny)
        if cached:
            os.makedirs(o['keep'], exist_ok=True)
            np.savez_compressed(cached, rgbn=rgbn)
        return rgbn

    # (the pictures asked for four at a time, ahead of the blocks' analysis: the service takes about half a minute a block)
    import concurrent.futures
    pool = concurrent.futures.ThreadPoolExecutor(max_workers=int(o.get('fetch', 4)))
    pending = [pool.submit(picture, bx, by) for bx, by in blocks]
    for k, (bx, by) in enumerate(blocks):
        x0, y0, nx, ny = window(bx, by)
        rgbn = pending[k].result()
        pending[k] = None
        # pixel centres (rows north to south)
        xs = x0 + (np.arange(nx) + 0.5) * CELL
        ys = y0 + ny * CELL - (np.arange(ny) + 0.5) * CELL
        lat, lon = pr.latlon(xs[None, :], ys[:, None])
        lat, lon = np.broadcast_to(lat, (ny, nx)), np.broadcast_to(lon, (ny, nx))
        heights = chm.heights(np.ascontiguousarray(lat), np.ascontiguousarray(lon))
        if not rgbn.any():
            continue
        # (the buildings round the block: their footprints drawn at its pixels, 6 m round them)
        built = Image.new('L', (nx, ny), 0)
        draw = ImageDraw.Draw(built)
        top = y0 + ny * CELL
        for fp in footprints_near(x0, y0, x0 + nx * CELL, top):
            draw.polygon([((px - x0) / CELL, (top - py) / CELL) for px, py in fp], fill=1)
        near_built = ndimage.binary_dilation(np.asarray(built, bool), iterations=6)
        canopy, block_trees, cls, shade = analyse(rgbn, heights, near_built)
        for px, py, h, crown, kind in block_trees:
            x, y = float(xs[px]), float(ys[py])
            if bx <= x < bx + BLOCK and by <= y < by + BLOCK and x < ax1 and y < ay1:
                trees.append((x, y, h, crown, kind))
        # the ground's colour and classes into the 4 m cells (the block's own pixels)
        inner = (xs[None, :] >= bx) & (xs[None, :] < bx + BLOCK) & (ys[:, None] >= by) & (ys[:, None] < by + BLOCK)
        gi = np.floor((xs - gx0) / GROUND).astype(np.int64)
        gj = np.floor((ys - gy0) / GROUND).astype(np.int64)
        GI, GJ = np.broadcast_to(gi[None, :], (ny, nx)), np.broadcast_to(gj[:, None], (ny, nx))
        ok = inner & (GI >= 0) & (GJ >= 0) & (GI < gnx) & (GJ < gny)
        seen = ok & (cls != 2) & (cls != 1) & ~shade  # (the ground's own colour: not in shade)
        for c in range(3):
            np.add.at(gsum[..., c], (GJ[seen], GI[seen]), rgbn[..., c][seen])
        np.add.at(gcount, (GJ[seen], GI[seen]), 1)
        for c in range(1, 7):
            m = ok & (cls == c)
            np.add.at(gclass[..., c], (GJ[m], GI[m]), 1)
        fi = np.floor((xs - ax0) / CELL).astype(np.int64)
        fj = np.floor((ys - ay0) / CELL).astype(np.int64)
        FI, FJ = np.broadcast_to(fi[None, :], (ny, nx)), np.broadcast_to(fj[:, None], (ny, nx))
        put = inner & (FI >= 0) & (FJ >= 0) & (FI < fnx) & (FJ < fny)
        fine[FJ[put], FI[put]] = cls[put]
        print(f'block {k + 1}/{len(blocks)} at {bx:.0f},{by:.0f}: {len(block_trees)} trees, canopy {canopy.mean():.0%} '
              f'({time.time() - started:.0f} s)', flush=True)
    # trees.bin
    with open(os.path.join(out, 'trees.bin'), 'wb') as f:
        f.write(b'WBTR' + struct.pack('<II', 1, len(trees)))
        for x, y, h, crown, kind in trees:
            f.write(struct.pack('<4fB3x', x, y, h, crown, kind))
    # cover.bin: each cell's commonest class, its ground's mean colour (where it shows)
    cover = np.argmax(gclass, axis=-1).astype(np.uint8)
    cover[gclass.sum(axis=-1) == 0] = 0
    colour = np.zeros((gny, gnx, 3), np.uint8)
    seen = gcount > 0
    colour[seen] = np.clip(gsum[seen] / gcount[seen][:, None], 0, 255).astype(np.uint8)
    with open(os.path.join(out, 'cover.bin'), 'wb') as f:
        f.write(b'WBPC' + struct.pack('<I3f2I', 1, gx0, gy0, GROUND, gnx, gny))
        f.write(np.concatenate([cover[..., None], colour], axis=-1).tobytes())
    with open(os.path.join(out, 'classes.bin'), 'wb') as f:
        f.write(b'WBCL' + struct.pack('<I3f2I', 1, ax0, ay0, CELL, fnx, fny))
        f.write(fine.tobytes())
    print(f'{len(trees)} trees ({sum(1 for t in trees if t[4] == 0)} evergreen, {sum(1 for t in trees if t[4] == 1)} bare); '
          f'cover: ' + ', '.join(f'{name} {np.mean(cover == c):.1%}' for c, name in enumerate(('unseen', 'water', 'canopy', 'grass', 'dry', 'bare', 'paved'))))


if __name__ == '__main__':
    main()
