"""Whether a street panorama's recorded heading can be trusted (research/phase0.md §70): Mapillary's compass_angle - what
the downloads carry - is the camera's own compass, and some 360-degree cameras' is a quarter turn off their picture's
middle (a car's camera mounted across, round 24's reviewers on every sheet of one creator's). The map tells the street's
axis where the picture was taken: per degree round the camera, how much of the ground 8-45 m out is road or car park on
the map, against how much of the picture's ground there (3-17 degrees under the horizon) looks like asphalt - grey, not
bright, not dark - the rotation that fits best. A car's camera faces along the street, ahead or back: a recorded heading
across the axis is wrong; one along it right as far as the map can tell (half a turn off is not told apart).

As a library: verdicts(world, rows) -> {id: 'along' | 'across' | 'oblique' | 'weak' | 'no road'} and trusted(verdicts, rows)
-> the ids whose heading to use: a creator's panoramas only when most of theirs the map could check are along (their
camera's compass is the picture's), and of those not the ones across.

usage: python -I tools/worldgen/panoheading.py --world <world.json> --metadata <metadata.csv>[,...]  (each creator's tally)
"""
import csv
import json
import math
import os
import sys

import numpy as np
from PIL import Image

DIST = np.arange(8.0, 45.0, 1.0)
AZ = np.arange(360) + 0.5
WALKS = ('footway', 'path', 'cycleway', 'steps', 'pedestrian', 'track', 'bridleway')


class Streets:
    """The world's carriageways (segments with their half width) and car parks, by 100 m cell."""

    def __init__(self, world):
        p = world['projection']
        self.to_game = lambda lat, lon: (p['x0'] + (lon - p['lon0']) * p['kx'], p['y0'] + (lat - p['lat0']) * p['ky'])
        segs = []
        for r in world.get('roads', []):
            if r.get('kind') in WALKS or r.get('bridge') or r.get('tunnel') or len(r.get('points', [])) < 2:
                continue
            q = np.array(r['points'], np.float64)[:, :2]
            lanes = max(1, int(r.get('forward', 1)) + int(r.get('backward', 1)))
            if r.get('kind') == 'service':
                lanes = min(lanes, 2)
            half = lanes * float(r.get('laneWidth', 3.5)) / 2 + 0.3
            for k in range(len(q) - 1):
                segs.append((q[k, 0], q[k, 1], q[k + 1, 0], q[k + 1, 1], half))
        self.seg = np.array(segs, np.float64) if segs else np.zeros((0, 5))
        self.lots = []
        for a in world.get('areas', []):
            if a.get('kind') in ('parking', 'paved'):
                for ring in a.get('outer', []):
                    if len(ring) >= 3:
                        self.lots.append(np.array(ring, np.float64)[:, :2])
        self.lot_mid = np.array([l.mean(0) for l in self.lots]) if self.lots else np.zeros((0, 2))

    def profile(self, x, y):
        """Per degree round (x, y) (compass), the share of the ground 8-45 m out that is road or car park."""
        s = self.seg
        near = s[(np.minimum(s[:, 0], s[:, 2]) < x + 60) & (np.maximum(s[:, 0], s[:, 2]) > x - 60) &
                 (np.minimum(s[:, 1], s[:, 3]) < y + 60) & (np.maximum(s[:, 1], s[:, 3]) > y - 60)]
        a = np.radians(AZ)
        px = x + np.sin(a)[:, None] * DIST[None, :]
        py = y + np.cos(a)[:, None] * DIST[None, :]
        on = np.zeros(px.shape, bool)
        for ax, ay, bx, by, half in near:
            dx, dy = bx - ax, by - ay
            L2 = dx * dx + dy * dy + 1e-9
            t = np.clip(((px - ax) * dx + (py - ay) * dy) / L2, 0, 1)
            on |= np.hypot(px - ax - t * dx, py - ay - t * dy) < half
        if len(self.lot_mid):
            for k in np.nonzero(np.hypot(self.lot_mid[:, 0] - x, self.lot_mid[:, 1] - y) < 200)[0]:
                on |= inside(self.lots[k], px, py)
        return on.mean(1)


def inside(poly, x, y):
    c = np.zeros(x.shape, bool)
    j = len(poly) - 1
    for i in range(len(poly)):
        xi, yi, xj, yj = poly[i, 0], poly[i, 1], poly[j, 0], poly[j, 1]
        c ^= ((yi > y) != (yj > y)) & (x < (xj - xi) * (y - yi) / (yj - yi + 1e-12) + xi)
        j = i
    return c


def asphalt(img):
    """Per degree round an equirectangular picture (from its left edge: its middle -180), the share of its ground 3-17
    degrees under the horizon - 8 to 45 m out from a camera 2-3 m up - that looks like asphalt."""
    a = np.asarray(img.convert('RGB').resize((1440, 720)), np.float32)
    band = a[int((0.5 + 3.2 / 180) * 720):int((0.5 + 17 / 180) * 720)]
    mx, mn = band.max(-1), band.min(-1)
    grey = ((mx - mn) / np.maximum(mx, 1) < 0.16) & (mx > 35) & (mx < 185)
    return grey.mean(0).reshape(360, 4).mean(1)


def axis(P, M):
    """Per compass bearing c of the picture's middle, how well its asphalt (P) fits the map's road (M): correlation."""
    Pz = (P - P.mean()) / (P.std() + 1e-6)
    idx = (np.arange(360)[None, :] + np.arange(360)[:, None] - 180) % 360  # (c, k): P's bin k looks at compass c - 180 + k
    m = M[idx]
    mz = (m - m.mean(1, keepdims=True)) / (m.std(1, keepdims=True) + 1e-6)
    return (Pz[None, :] * mz).mean(1)


def verdict(streets, x, y, img, heading):
    """'along', 'across', 'oblique' (the recorded heading against the street's axis there), 'weak' (no clear fit) or
    'no road' (too little street round it)."""
    M = streets.profile(x, y)
    if M.mean() < 0.08:
        return 'no road'
    sc = axis(asphalt(img), M)
    best = int(np.argmax(sc))
    if sc[best] < 0.45:
        return 'weak'
    e = (best - heading) % 180
    e = min(e, 180 - e)
    return 'along' if e < 25 else 'across' if e > 65 else 'oblique'


def verdicts(world, rows):
    """{id: verdict} for the panoramas among rows (dicts with id, lat, lon, heading, pano, path) inside the world."""
    streets = Streets(world)
    x0, y0, x1, y1 = world['min'][0], world['min'][1], world['max'][0], world['max'][1]
    out = {}
    for r in rows:
        if r.get('pano') != 'True' or not os.path.exists(r.get('path', '')):
            continue
        try:
            x, y = streets.to_game(float(r['lat']), float(r['lon']))
            h = float(r['heading'])
        except (KeyError, ValueError):
            continue
        if not (x0 <= x < x1 and y0 <= y < y1):
            continue
        out[r['id']] = verdict(streets, x, y, Image.open(r['path']), h)
    return out


def trusted(found, rows):
    """The panoramas' ids whose recorded heading to use: a creator's only when at least 70 % of theirs the map checked
    (along, across or oblique - 5 at least) are along, and of those not the ones across or oblique."""
    tally = {}
    for r in rows:
        v = found.get(r['id'])
        if v in ('along', 'across', 'oblique'):
            t = tally.setdefault(r.get('creator', ''), [0, 0])
            t[0] += v == 'along'
            t[1] += 1
    good = {c for c, (a, n) in tally.items() if n >= 5 and a >= 0.7 * n}
    return {r['id'] for r in rows if r['id'] in found and r.get('creator', '') in good and found[r['id']] not in ('across', 'oblique')}


def read_rows(metadata):
    rows = []
    for meta in metadata.split(','):
        with open(meta, encoding='utf-8') as f:
            for r in csv.DictReader(f):
                r['path'] = os.path.join(os.path.dirname(meta), r.get('file', ''))
                rows.append(r)
    return rows


def main():
    o = dict(zip(sys.argv[1::2], sys.argv[2::2]))
    if '--world' not in o or '--metadata' not in o:
        sys.exit(__doc__)
    with open(o['--world'], encoding='utf-8') as f:
        world = json.load(f)
    rows = read_rows(o['--metadata'])
    found = verdicts(world, rows)
    ok = trusted(found, rows)
    tally = {}
    for r in rows:
        if r['id'] in found:
            tally.setdefault(r.get('creator', ''), {}).setdefault(found[r['id']], 0)
            tally[r.get('creator', '')][found[r['id']]] += 1
    for c, t in sorted(tally.items(), key=lambda kv: -sum(kv[1].values())):
        print(f'{c:20s} {t}')
    print(f'{len(ok)} of {len(found)} panoramas\' headings trusted')


if __name__ == '__main__':
    main()
