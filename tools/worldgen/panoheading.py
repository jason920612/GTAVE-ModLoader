"""Whether a street panorama's recorded heading can be trusted (research/phase0.md §70): Mapillary's compass_angle - what
the downloads carry - is the camera's own compass, and some 360-degree cameras' is a quarter turn off their picture's
middle (a car's camera mounted across, round 24's reviewers on every sheet of one creator's). The map tells the street's
axis where the picture was taken: per degree round the camera, how much of the ground 8-45 m out is road or car park on
the map, against how much of the picture's ground there (3-17 degrees under the horizon) looks like asphalt - grey, not
bright, not dark - the rotation that fits best. A car's camera faces along the street, ahead or back: a recorded heading
across the axis is wrong; one along it right as far as the map can tell (half a turn off is not told apart).

Where some of a creator's panoramas' middles have been measured (poses.json beside the sets' folders: {"poses": [{"id",
"middle", "confidence"}, ...]} - read off the aerial pictures, the map and the sun), the creator's rule: the recorded
heading turned (h + c) or mirrored (c - h: a camera counting its angle the other way round), fitted to them - kept when
at least five were measured and four in five lie within 10 degrees of it. A car's camera in Orlando: all of twelve
within 4 degrees of 90 - h (its middle its rear, its angle anticlockwise from east), every one across the street as
recorded.

As a library: verdicts(world, rows) -> {id: 'along' | 'across' | 'oblique' | 'weak' | 'no road'} and trusted(verdicts, rows)
-> the ids whose heading to use: a creator's panoramas only when most of theirs the map could check are along (their
camera's compass is the picture's), and of those not the ones across; heading(row) the heading to use - the creator's
rule's where it has one (read_rows reads the rules with the rows).

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


def rules(rows, poses):
    """{creator: (sign, c, rear)} - the heading to use sign * h + c, and whether the middle looks back (most of the measured
    ones a car's rear view) - for the creators with five measured middles or more of which four in five lie within 10
    degrees of the best rule (h + c or c - h)."""
    by = {}
    back = {}
    creator = {r['id']: r.get('creator', '') for r in rows}
    for p in poses:
        if p.get('id') in creator and p.get('confidence') in ('high', 'medium'):
            r = next(r for r in rows if r['id'] == p['id'])
            try:
                by.setdefault(creator[p['id']], []).append((float(r['heading']), float(p['middle'])))
                back.setdefault(creator[p['id']], []).append(p.get('rear') is True)
            except (KeyError, ValueError):
                continue
    out = {}
    for c, pairs in by.items():
        if len(pairs) < 5:
            continue
        best = None
        for sign in (1, -1):
            # (c: the circular mean of middle - sign * h; then the share within 10 degrees of it)
            d = np.radians([m - sign * h for h, m in pairs])
            off = math.degrees(math.atan2(np.sin(d).mean(), np.cos(d).mean())) % 360
            err = [abs((m - sign * h - off + 180) % 360 - 180) for h, m in pairs]
            share = sum(e <= 10 for e in err) / len(err)
            if best is None or share > best[0]:
                best = (share, sign, off)
        if best[0] >= 0.8:
            out[c] = (best[1], best[2], sum(back[c]) > len(back[c]) / 2)
    return out


RULES = {}


def heading(r):
    """The heading to use for a row: its creator's rule's (rules()) where there is one, else the recorded."""
    h = float(r['heading'])
    rule = RULES.get(r.get('creator', ''))
    return (rule[0] * h + rule[1]) % 360 if rule else h


def looks_back(r):
    """Whether the row's picture's middle looks back along the way its car went (its creator's rule says so)."""
    rule = RULES.get(r.get('creator', ''))
    return bool(rule and rule[2])


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
            h = heading(r)
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
    good = {c for c, (a, n) in tally.items() if n >= 5 and a >= 0.7 * n} | set(RULES)
    return {r['id'] for r in rows if r['id'] in found and r.get('creator', '') in good and found[r['id']] not in ('across', 'oblique')}


def read_rows(metadata):
    """The sets' rows (with each picture's path), and the creators' rules (RULES) from the measured middles beside them."""
    rows = []
    poses = []
    for meta in metadata.split(','):
        with open(meta, encoding='utf-8') as f:
            for r in csv.DictReader(f):
                r['path'] = os.path.join(os.path.dirname(meta), r.get('file', ''))
                rows.append(r)
        measured = os.path.join(os.path.dirname(os.path.dirname(os.path.abspath(meta))), 'poses.json')
        if os.path.exists(measured) and not poses:
            with open(measured, encoding='utf-8') as f:
                poses = json.load(f).get('poses', [])
    RULES.clear()
    RULES.update(rules(rows, poses))
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
        rule = RULES.get(c)
        print(f'{c:20s} {t}' + (f'  (heading {"" if rule[0] > 0 else "-"}h + {rule[1]:.0f} by the measured middles)' if rule else ''))
    print(f'{len(ok)} of {len(found)} panoramas\' headings trusted')


if __name__ == '__main__':
    main()
