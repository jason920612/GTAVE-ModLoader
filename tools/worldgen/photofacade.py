"""The buildings' walls as the street pictures show them (research/phase0.md §63): each street picture (Mapillary, CC BY-SA
4.0: its creator named in the output) looked through from where it was taken - its place and compass heading, a
phone's field of view or a panorama's whole round -, each column of the picture to the first wall of the world's
buildings it meets, the wall's band from above the cars and the bushes to under the eaves sampled; leaves, sky and deep
shadow left out. Each building seen: the median colour of its wall and how much of it looks like brick (red to orange,
well saturated, middling bright), from every picture that saw it - worldgen build --facades picks its walls' style and
colour by them. A panorama only when the map backs its recorded heading (panoheading.py: its creator's mostly along the
streets they were taken in, and not this one across): Mapillary's is the camera's own compass, on some 360-degree cameras
a quarter turn off the picture's middle.

usage: python -I tools/worldgen/photofacade.py --world <world.json> --metadata <metadata.csv>[,<metadata.csv>...] --out <facades.json>
       [--fov 50] [--reach 45] [--eye 1.8] [--sheet <dir>]
  --fov: a flat picture's vertical field of view (degrees; a phone's 4:3 about 50), panoramas are equirectangular
  --reach: walls farther than this (m) left out; --eye: the camera's height over the ground (m)
  --sheet: a picture per 40 buildings seen, for checking: each building's wall colour beside the picture it came from
  --debug <dir>: the first 30 pictures with the bands sampled drawn over them, each building's number
"""
import csv, json, math, os, sys

import numpy as np
from PIL import Image, ImageDraw

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import panoheading


def options(argv):
    o = {'fov': '50', 'reach': '45', 'eye': '1.8'}
    for i in range(0, len(argv) - 1, 2):
        if not argv[i].startswith('--'):
            sys.exit(__doc__)
        o[argv[i][2:]] = argv[i + 1]
    return o


class Walls:
    """The world's buildings' outlines as wall segments, by 50 m cell."""

    def __init__(self, w):
        self.buildings = w['buildings']
        self.cells = {}
        segs = []
        for bi, b in enumerate(self.buildings):
            ring = b['footprint']
            if len(ring) < 3:
                continue
            for k in range(len(ring)):
                a, c = ring[k], ring[(k + 1) % len(ring)]
                segs.append((a[0], a[1], c[0], c[1], bi))
        self.seg = np.array(segs, np.float64) if segs else np.zeros((0, 5))
        for si, s in enumerate(segs):
            for gi in range(int(min(s[0], s[2]) // 50), int(max(s[0], s[2]) // 50) + 1):
                for gj in range(int(min(s[1], s[3]) // 50), int(max(s[1], s[3]) // 50) + 1):
                    self.cells.setdefault((gi, gj), []).append(si)

    def near(self, x, y, reach):
        idx = set()
        for gi in range(int((x - reach) // 50), int((x + reach) // 50) + 1):
            for gj in range(int((y - reach) // 50), int((y + reach) // 50) + 1):
                idx.update(self.cells.get((gi, gj), ()))
        return self.seg[sorted(idx)] if idx else np.zeros((0, 5))


def first_hits(seg, x, y, az):
    """For rays from (x, y) along azimuths az (radians, compass: clockwise from north): the distance to the first wall and
    its building (inf, -1: none)."""
    dx, dy = np.sin(az)[:, None], np.cos(az)[:, None]
    ax, ay, bx, by = seg[:, 0][None], seg[:, 1][None], seg[:, 2][None], seg[:, 3][None]
    ex, ey = bx - ax, by - ay
    den = dx * ey - dy * ex
    with np.errstate(divide='ignore', invalid='ignore'):
        t = ((ax - x) * ey - (ay - y) * ex) / den  # (along the ray)
        u = ((ax - x) * dy - (ay - y) * dx) / den  # (along the wall)
    ok = (np.abs(den) > 1e-9) & (t > 0.5) & (u >= 0) & (u <= 1)
    t = np.where(ok, t, np.inf)
    k = np.argmin(t, axis=1)
    d = t[np.arange(len(az)), k]
    b = np.where(np.isfinite(d), seg[k, 4].astype(int), -1)
    return d, b


def main():
    o = options(sys.argv[1:])
    if 'world' not in o or 'metadata' not in o or 'out' not in o:
        sys.exit(__doc__)
    with open(o['world'], encoding='utf-8') as f:
        w = json.load(f)
    p = w['projection']
    to_game = lambda lat, lon: (p['x0'] + (lon - p['lon0']) * p['kx'], p['y0'] + (lat - p['lat0']) * p['ky'])
    x0, y0, x1, y1 = w['min'][0], w['min'][1], w['max'][0], w['max'][1]
    walls = Walls(w)
    fov, reach, eye = math.radians(float(o['fov'])), float(o['reach']), float(o['eye'])
    seen = {}  # building -> list of (rgb pixels, picture id)
    creators = {}
    pictures = 0
    # (the panoramas whose recorded heading the map backs)
    every = panoheading.read_rows(o['metadata'])
    found = panoheading.verdicts(w, every)
    ok = panoheading.trusted(found, every)
    if found:
        print(f'panoramas: {len(ok)} of {len(found)} here with a heading the streets back; {len(found) - len(ok)} left out '
              f'(their camera\'s compass across the street, or their creator\'s mostly)')
    for meta in o['metadata'].split(','):
        folder = os.path.dirname(meta)
        with open(meta, encoding='utf-8') as f:
            rows = list(csv.DictReader(f))
        for r in rows:
            if r.get('pano') == 'True' and r.get('id') not in ok:
                continue
            try:
                lat, lon, heading = float(r['lat']), float(r['lon']), float(r['heading'])
            except (KeyError, ValueError):
                continue
            x, y = to_game(lat, lon)
            if not (x0 <= x < x1 and y0 <= y < y1):
                continue
            seg = walls.near(x, y, reach)
            if not len(seg):
                continue
            path = os.path.join(folder, r['file'])
            if not os.path.exists(path):
                continue
            im = Image.open(path).convert('RGB')
            pano = r.get('pano') == 'True'
            W = 2048 if pano else 1024
            im = im.resize((W, round(W * im.height / im.width)))
            a = np.asarray(im, np.float32)
            H = a.shape[0]
            cols = np.arange(2, W, 4)
            if pano:
                az = np.radians(heading) + (cols / W - 0.5) * 2 * math.pi
            else:
                fpx = (H / 2) / math.tan(fov / 2)
                az = np.radians(heading) + np.arctan((cols - W / 2) / fpx)
            d, b = first_hits(seg, x, y, az)
            hit = (b >= 0) & (d <= reach) & (d >= 3)
            if not hit.any():
                continue
            pictures += 1
            creators[r.get('creator', '')] = creators.get(r.get('creator', ''), 0) + 1
            # (each building's run of columns: its middle part only - a heading a few degrees off puts its edges on the
            # neighbour's wall or the gap between)
            runs = {}
            for ci in np.nonzero(hit)[0]:
                runs.setdefault(int(b[ci]), []).append(ci)
            debug = None
            if 'debug' in o and pictures <= 30:
                debug = im.copy()
                dd = ImageDraw.Draw(debug)
            for bi, cis in runs.items():
                if len(cis) < 4:
                    continue
                cis = sorted(cis)
                trim = len(cis) // 5
                cis = cis[trim:len(cis) - trim]
                height = float(w['buildings'][bi].get('height') or 4.5)
                # (a house of one floor: its walls to the eaves 2.6 m up - the map's height is the ridge's, and the band
                # up to it was the roof: a green metal roof taken for leaves, the house not seen)
                lo, hi = (0.8, min(height - 0.4, 2.6)) if height < 6 else (2.5, height - 0.5)
                # (the band's middle half: a wall's place in a picture is a metre or two out - the roof, the bushes)
                lo, hi = lo + 0.25 * (hi - lo), hi - 0.25 * (hi - lo)
                if hi <= lo:
                    continue
                px = []
                for ci in cis:
                    dist = d[ci]
                    e0, e1 = math.atan2(lo - eye, dist), math.atan2(hi - eye, dist)
                    if pano:
                        v0, v1 = (0.5 - e1 / math.pi) * H, (0.5 - e0 / math.pi) * H
                    else:
                        v0, v1 = H / 2 - math.tan(e1) * fpx, H / 2 - math.tan(e0) * fpx
                    v0, v1 = max(0, int(v0)), min(H, int(v1) + 1)
                    if v1 - v0 < 2:
                        continue
                    col = a[v0:v1, cols[ci]]
                    px.append(col)
                    if debug is not None:
                        dd.line((int(cols[ci]), v0, int(cols[ci]), v1), fill=(255, 0, 255) if bi % 2 else (0, 255, 255), width=2)
                if not px:
                    continue
                px = np.concatenate(px)
                rr, gg, bb = px[:, 0], px[:, 1], px[:, 2]
                mx, mn = px.max(axis=1), px.min(axis=1)
                sat = (mx - mn) / np.maximum(mx, 1)
                # (leaves: green, or in the shade a dark grey-green)
                leaves = ((gg > rr * 1.03) & (gg > bb * 1.03) & (sat > 0.12)) | ((gg >= rr) & (gg >= bb * 0.95) & (mx < 95))
                sky = (bb > rr + 15) & (bb >= gg) & (mx > 120)
                dark = mx < 22
                keep = ~(leaves | sky | dark)
                if keep.sum() < 12:
                    continue
                seen.setdefault(bi, []).append((px[keep], r['id']))
                if debug is not None:
                    dd.text((int(cols[cis[len(cis) // 2]]), 10), f'#{bi}', fill=(255, 255, 0))
            if debug is not None:
                os.makedirs(o['debug'], exist_ok=True)
                debug.save(os.path.join(o['debug'], f'{r["id"]}.jpg'))
    out = []
    for bi, obs in seen.items():
        px = np.concatenate([q for q, _ in obs])
        # (the wall's colour: the pixels between the median and the 90th percentile of brightness - its lit part, not the
        # window glass, the eaves' shadow or a shaded corner)
        lum = px.sum(axis=1)
        q50, q90 = np.percentile(lum, [50, 90])
        lit = (lum >= q50) & (lum <= q90)
        rgb = px[lit].mean(axis=0) if lit.any() else np.median(px, axis=0)
        mx, mn = px.max(axis=1), px.min(axis=1)
        sat = (mx - mn) / np.maximum(mx, 1)
        r, g, b = px[:, 0], px[:, 1], px[:, 2]
        # (brick: red to orange - red well over blue, green between -, saturated, neither bright nor black; by ratios, not
        # differences: a brick house in an overcast picture's shade, (76, 63, 52), had failed red 25 over blue - tan siding's
        # green is nearer its red, 0.9 of it)
        brick = (r > b * 1.25) & (g < r * 0.88) & (g > b * 0.85) & (sat > 0.2) & (sat < 0.8) & (mx > 40) & (mx < 210)
        out.append({'building': bi, 'colour': [int(round(c)) for c in rgb], 'brick': round(float(brick.mean()), 3), 'samples': int(len(px)),
                    'pictures': sorted({pid for _, pid in obs})[:8]})
    out.sort(key=lambda e: e['building'])
    credit = 'Street pictures: Mapillary contributors (CC BY-SA 4.0): ' + ', '.join(f'{c} ({n})' for c, n in sorted(creators.items(), key=lambda kv: -kv[1]))
    with open(o['out'], 'w', encoding='utf-8') as f:
        json.dump({'attribution': credit, 'buildings': out}, f, ensure_ascii=False)
    print(f'{pictures} pictures looked through, {len(out)} buildings seen ({sum(1 for e in out if e["brick"] > 0.4)} brick)')
    if 'sheet' in o:
        sheet(o['sheet'], out, w)


def sheet(folder, out, w):
    """Swatches for checking: each building seen, its colour and brick share."""
    os.makedirs(folder, exist_ok=True)
    per = 40
    for k in range(0, min(len(out), 400), per):
        img = Image.new('RGB', (600, 20 * per), (255, 255, 255))
        dr = ImageDraw.Draw(img)
        for i, e in enumerate(out[k:k + per]):
            dr.rectangle((0, i * 20, 60, i * 20 + 18), fill=tuple(e['colour']))
            b = w['buildings'][e['building']]
            dr.text((70, i * 20 + 3), f"#{e['building']} {b.get('kind', '')} h {b.get('height', 0):.0f} brick {e['brick']:.2f} n {e['samples']} {e['pictures'][0]}",
                    fill=(0, 0, 0))
        img.save(os.path.join(folder, f'facades_{k // per:02d}.png'))


if __name__ == '__main__':
    main()
