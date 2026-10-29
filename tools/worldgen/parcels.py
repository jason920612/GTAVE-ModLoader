"""What the property rolls say about each building of a land (research/phase0.md §63.1): Florida's Department of Revenue
publishes every county's parcels (its data portal's Map Data, "<county>_<year>Ppar.zip": the parcel polygons joined with
the real property roll, the NAL) - public records. For each of the world's buildings, the parcel its middle lies in: the
land use code (DOR_UC: 000 vacant residential, 001 single family, 002 mobile home, 003/008 multi-family, 004 condominium,
010-039 commercial, 040-049 industrial, 070-079 institutional, 080-089 governmental...), the year its main structure was
built, its living area (all floors), the number of buildings and of homes on the parcel, the improvement quality and the
construction class (commercial: 3 masonry, 4 wood frame...). From the living area and the footprints on the parcel: its
storeys (the living area over the main building's footprint - a garage under the same roof counts in the footprint, not
in the living area: under 1.3 one storey). Owners' names and addresses are not read.

usage (in a Python with pyshp and pyproj; keep the data outside the repository):
  parcels.py fetch --out <dir> [--year 2026F]   every county's parcels (resumable) and <dir>/index.json: each county's box
                                                 (longitude, latitude) and shapefile
  parcels.py join  --world <world.json> --out <parcels.json> (--parcels <shapefile without .shp>[,...] | --index <index.json>:
                   the counties whose box meets the world's)
Out: {"attribution": ..., "buildings": [{"building": index in world.json, "use": "001", "year": 1962, "living": m2, "count": n,
      "units": n, "quality": 1-6, "class": 0-5, "storeys": n, "main": the parcel's biggest, "lot": [[x, y], ...]}, ...]}
     ("lot": a house's - single family, mobile home, small multi-family - parcel under 4000 m2, an apartment block's
     (multi-family, condominium, cooperative, retirement home) under 2 ha, its outline in the world's
     metres to 0.1 m without its closing point and the points on a straight line; the main building's only)
"""
import json, math, os, sys, time, urllib.parse, urllib.request, zipfile

FIELDS = ('DOR_UC', 'ACT_YR_BLT', 'TOT_LVG_AR', 'NO_BULDNG', 'NO_RES_UNT', 'IMP_QUAL', 'CONST_CLAS')
PORTAL = 'https://floridarevenue.com/property/dataportal'
AGENT = {'User-Agent': 'Mozilla/5.0 (GTAVE-ModLoader worldgen; property rolls for a game map of Florida)'}


def options(argv):
    o = {}
    for i in range(0, len(argv) - 1, 2):
        if not argv[i].startswith('--'):
            sys.exit(__doc__)
        o[argv[i][2:]] = argv[i + 1]
    return o


def inside(ring, x, y):
    c = False
    n = len(ring)
    j = n - 1
    for i in range(n):
        xi, yi = ring[i]
        xj, yj = ring[j]
        if (yi > y) != (yj > y) and x < (xj - xi) * (y - yi) / (yj - yi) + xi:
            c = not c
        j = i
    return c


def area(ring):
    s = 0.0
    for i in range(len(ring)):
        x0, y0 = ring[i]
        x1, y1 = ring[(i + 1) % len(ring)]
        s += x0 * y1 - x1 * y0
    return abs(s) / 2


def simplify(ring, tolerance=0.15):
    """A ring without its closing point and the points within tolerance of the line through their neighbours, to 0.1 m."""
    pts = [tuple(q) for q in ring]
    if len(pts) > 1 and pts[0] == pts[-1]:
        pts = pts[:-1]
    changed = True
    while changed and len(pts) > 3:
        changed = False
        for i in range(len(pts)):
            a, b, c = pts[i - 1], pts[i], pts[(i + 1) % len(pts)]
            dx, dy = c[0] - a[0], c[1] - a[1]
            l = math.hypot(dx, dy)
            if l < 1e-6 or abs((b[0] - a[0]) * dy - (b[1] - a[1]) * dx) / l < tolerance:
                del pts[i]
                changed = True
                break
    return [[round(q[0], 1), round(q[1], 1)] for q in pts]


def simplify(ring, tolerance=0.15):
    """A ring without its closing point and the points within tolerance of the line through their neighbours, to 0.1 m."""
    pts = [tuple(q) for q in ring]
    if len(pts) > 1 and pts[0] == pts[-1]:
        pts = pts[:-1]
    changed = True
    while changed and len(pts) > 3:
        changed = False
        for i in range(len(pts)):
            a, b, c = pts[i - 1], pts[i], pts[(i + 1) % len(pts)]
            dx, dy = c[0] - a[0], c[1] - a[1]
            l = math.hypot(dx, dy)
            if l < 1e-6 or abs((b[0] - a[0]) * dy - (b[1] - a[1]) * dx) / l < tolerance:
                del pts[i]
                changed = True
                break
    return [[round(q[0], 1), round(q[1], 1)] for q in pts]


def get(url, tries=4):
    for k in range(tries):
        try:
            req = urllib.request.Request(url, headers={**AGENT, 'Accept': 'application/json;odata=nometadata'})
            with urllib.request.urlopen(req, timeout=120) as r:
                return r.read()
        except OSError as e:
            if k == tries - 1:
                raise
            print(f'{url}: {e}, again', flush=True)
            time.sleep(5 * (k + 1))


def fetch(o):
    """Every county's parcel shapefile from the portal's Map Data folder (SharePoint's REST listing), unpacked, and the
    index of their boxes."""
    import shapefile, pyproj
    out = o['out']
    year = o.get('year', '2026F')
    os.makedirs(out, exist_ok=True)
    folder = f"/property/dataportal/Documents/PTO Data Portal/Map Data/{year}/{year} PAR"
    listing = json.loads(get(f"{PORTAL}/_api/web/GetFolderByServerRelativeUrl('{urllib.parse.quote(folder)}')/Files?$select=Name,Length"))
    files = sorted((f['Name'], int(f.get('Length') or 0)) for f in listing['value'] if f['Name'].lower().endswith('.zip'))
    print(f'{len(files)} counties in {year}', flush=True)
    index = {}
    for name, length in files:
        zpath = os.path.join(out, name)
        if not (os.path.exists(zpath) and os.path.getsize(zpath) == length):
            data = get(f"{PORTAL}/Documents/PTO%20Data%20Portal/Map%20Data/{year}/{urllib.parse.quote(year + ' PAR')}/{urllib.parse.quote(name)}")
            with open(zpath + '.part', 'wb') as f:
                f.write(data)
            os.replace(zpath + '.part', zpath)
            print(f'{name}: {len(data) / 1e6:.0f} MB', flush=True)
        stem = name[:-4]
        folder_out = os.path.join(out, stem)
        if not os.path.isdir(folder_out):
            with zipfile.ZipFile(zpath) as z:
                z.extractall(folder_out)
        shp = [os.path.join(dp, f) for dp, _, fs in os.walk(folder_out) for f in fs if f.lower().endswith('.shp')]
        if not shp:
            print(f'{name}: no shapefile in it', flush=True)
            continue
        path = shp[0][:-4]
        r = shapefile.Reader(path)
        x0, y0, x1, y1 = r.bbox
        with open(path + '.prj', encoding='utf-8') as f:
            tr = pyproj.Transformer.from_crs(pyproj.CRS.from_wkt(f.read()), 'EPSG:4326', always_xy=True)
        lon, lat = tr.transform([x0, x1, x0, x1], [y0, y0, y1, y1])
        index[stem] = {'shapefile': path, 'box': [min(lon), min(lat), max(lon), max(lat)], 'parcels': r.numRecords}
        r.close()
    with open(os.path.join(out, 'index.json'), 'w', encoding='utf-8') as f:
        json.dump(index, f, indent=1)
    print(f'{len(index)} counties indexed in {os.path.join(out, "index.json")}', flush=True)


def join(o):
    import shapefile, pyproj
    with open(o['world'], encoding='utf-8') as f:
        w = json.load(f)
    p = w['projection']
    x0, y0, x1, y1 = w['min'][0], w['min'][1], w['max'][0], w['max'][1]
    if 'parcels' in o:
        paths = o['parcels'].split(',')
    else:
        # (the counties whose box meets the world's)
        with open(o['index'], encoding='utf-8') as f:
            index = json.load(f)
        s, wl = p['lat0'] + (y0 - p['y0']) / p['ky'], p['lon0'] + (x0 - p['x0']) / p['kx']
        n, e = p['lat0'] + (y1 - p['y0']) / p['ky'], p['lon0'] + (x1 - p['x0']) / p['kx']
        paths = [c['shapefile'] for c in index.values() if c['box'][0] <= e and c['box'][2] >= wl and c['box'][1] <= n and c['box'][3] >= s]
    # the buildings' middles, by 100 m cell
    mids, cells = [], {}
    for bi, b in enumerate(w['buildings']):
        fp = b['footprint']
        if len(fp) < 3:
            mids.append(None)
            continue
        mx = sum(q[0] for q in fp) / len(fp)
        my = sum(q[1] for q in fp) / len(fp)
        mids.append((mx, my, area(fp)))
        cells.setdefault((int(mx // 100), int(my // 100)), []).append(bi)
    found = {}
    parcels_read = 0
    for path in paths:
        r = shapefile.Reader(path)
        with open(path + '.prj', encoding='utf-8') as f:
            tr = pyproj.Transformer.from_crs(pyproj.CRS.from_wkt(f.read()), 'EPSG:4326', always_xy=True)
        names = [f[0] for f in r.fields[1:]]
        # (a county's other downloads - Miami-Dade's condominium points - are no property roll: left out)
        if any(k not in names for k in FIELDS):
            print(f'{os.path.basename(path)}: no {", ".join(k for k in FIELDS if k not in names)} - not a property roll, left out', flush=True)
            continue
        idx = [names.index(k) for k in FIELDS]
        for k in range(r.numRecords):
            # (a record whose polygon has no points - some counties' rolls have them - is no parcel: the reader will not
            # make its shape, and iterating stops there)
            try:
                shape = r.shape(k)
            except shapefile.ShapefileException:
                continue
            pts = shape.points
            if len(pts) < 3:
                continue
            # (the box first: a parcel outside the land is not looked at)
            xs = [q[0] for q in pts]
            ys = [q[1] for q in pts]
            lon, lat = tr.transform([min(xs), max(xs)], [min(ys), max(ys)])
            gx = [p['x0'] + (v - p['lon0']) * p['kx'] for v in lon]
            gy = [p['y0'] + (v - p['lat0']) * p['ky'] for v in lat]
            if min(gx) > x1 or max(gx) < x0 or min(gy) > y1 or max(gy) < y0:
                continue
            parcels_read += 1
            lon, lat = tr.transform(xs, ys)
            # (each part a ring; the parts' even-odd inside: holes too)
            ring = [(p['x0'] + (lo - p['lon0']) * p['kx'], p['y0'] + (la - p['lat0']) * p['ky']) for lo, la in zip(lon, lat)]
            parts = list(shape.parts) + [len(ring)]
            rings = [ring[parts[k]:parts[k + 1]] for k in range(len(parts) - 1)]
            bx0, bx1 = min(q[0] for q in ring), max(q[0] for q in ring)
            by0, by1 = min(q[1] for q in ring), max(q[1] for q in ring)
            members = []
            for ci in range(int(bx0 // 100), int(bx1 // 100) + 1):
                for cj in range(int(by0 // 100), int(by1 // 100) + 1):
                    for bi in cells.get((ci, cj), ()):
                        mx, my, _ = mids[bi]
                        if bx0 <= mx <= bx1 and by0 <= my <= by1 and sum(inside(rg, mx, my) for rg in rings) % 2:
                            members.append(bi)
            if not members:
                continue
            rec = r.record(k)
            use, year, living, count, units, quality, cls = (rec[k] for k in idx)
            living = float(living or 0) * 0.09290304  # (square feet)
            members.sort(key=lambda bi: -mids[bi][2])
            main_area = mids[members[0]][2]
            # (homes spread over several buildings - apartments, condominiums - by all their footprints; else the main one's)
            spread = (count or 0) > 1 and str(use) in ('003', '004', '005', '006', '008')
            base = sum(mids[bi][2] for bi in members) if spread else main_area
            ratio = living / base if base > 0 else 0
            storeys = 0 if living <= 0 else 1 if ratio < 1.3 else 2 if ratio < 2.4 else max(3, round(ratio / 0.9))
            # (a house's lot, for its yard's fence: its outline - the first ring, the outer one -)
            # (and an apartment block's - the multi-family homes, condominiums, cooperatives, retirement homes -: up to 2 ha)
            house_lot = str(use) in ('001', '002', '007', '008') and area(rings[0]) < 4000
            block_lot = str(use) in ('003', '004', '005', '006') and area(rings[0]) < 20000
            lot = simplify(rings[0]) if (house_lot or block_lot) and len(rings[0]) >= 4 else None
            for k, bi in enumerate(members):
                found[bi] = {'building': bi, 'use': str(use or ''), 'year': int(year or 0), 'living': round(living, 1), 'count': int(count or 0),
                             'units': int(units or 0), 'quality': int(quality) if str(quality).strip().isdigit() else 0, 'class': int(cls or 0),
                             'storeys': storeys if (k == 0 or spread) else 1, 'main': k == 0}
                if k == 0 and lot and len(lot) >= 3:
                    found[bi]['lot'] = lot
        r.close()
    out = sorted(found.values(), key=lambda e: e['building'])
    with open(o['out'], 'w', encoding='utf-8') as f:
        json.dump({'attribution': 'Parcels and property rolls: Florida Department of Revenue and the county property appraisers (public records)',
                   'buildings': out}, f)
    from collections import Counter
    uses = Counter(e['use'] for e in out)
    st = Counter(e['storeys'] for e in out if e['main'] and e['use'] == '001')
    print(f'{len(paths)} counties, {parcels_read} parcels over the land, {len(out)} of {len(w["buildings"])} buildings on one; '
          f'uses {uses.most_common(8)}; single family storeys {sorted(st.items())}')


def main():
    if len(sys.argv) < 2 or sys.argv[1] not in ('fetch', 'join'):
        sys.exit(__doc__)
    o = options(sys.argv[2:])
    if sys.argv[1] == 'fetch':
        if 'out' not in o:
            sys.exit(__doc__)
        fetch(o)
    else:
        if 'world' not in o or 'out' not in o or ('parcels' not in o and 'index' not in o):
            sys.exit(__doc__)
        join(o)


if __name__ == '__main__':
    main()
