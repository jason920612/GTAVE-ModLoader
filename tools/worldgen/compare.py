"""Real pictures beside the game's (research/phase0.md §62): for a land (a pack in ModLoader\\mods with its land.json),
the USDA's aerial pictures (NAIP through The National Map; public domain) of boxes of the land beside the game seen
from straight above, and the geotagged ground photos Wikimedia Commons has there (each under its own licence, kept with
the pictures, outside the repository) beside the game at the photo's place. The game runs (tools/gtactl.py) with the
land there; the scripted camera keeps noon and a clear sky.

usage: python -I tools/worldgen/compare.py aerial --land <name> --out <dir> [--at x,y;x,y...] [--count 6] [--size 240]
       python -I tools/worldgen/compare.py photos --land <name> --out <dir> [--limit 12]
       python -I tools/worldgen/compare.py mapillary --land <name> --metadata <metadata.csv> --out <dir> [--limit 12] [--seed 1]
  aerial: boxes --size m tall (16:9) at --at, else --count of them spread over the land's ground: <dir>/aerial_<x>_<y>.png
          (the picture above, the game below) and a line each in <dir>/aerial.tsv: each picture's share of tree-dark
          green, light green, bare or dry, grey and dark, and their mean colours
  photos: <dir>/photo_<k>.png: the photo, the game at its place 1.7 m up (its direction from the photo's EXIF, else the
          four ways round); <dir>/photos.json: each photo's title, licence, author, place
  mapillary: street pictures someone downloaded from Mapillary (CC BY-SA 4.0: their creators named on each sheet),
          not panoramas, at their places and headings: <dir>/street_<k>_<id>.png
       python -I tools/worldgen/compare.py pano --land <name> --metadata <metadata.csv>[,...] --out <dir> [--limit 12] [--seed 1]
          [--spacing 80] [--views 0,90,-90] [--poses <poses.json>]
  pano: the downloads' panoramas - where the comparison is exact: the game's camera at the picture's place (2 m up), and
          out of the panorama the very view the game's camera has (its field of view, 16:9) towards the picture's heading
          and the --views offsets (degrees: ahead, right, left); the panorama's own heading error (a few degrees) and tilt
          taken out by fitting its skyline to the game's (yaw within 15, pitch within 4 degrees): <dir>/pano_<k>_<id>_<view>.png
          (the picture above, the game below) and <dir>/pano.json (each sheet's place, heading, the fit and how well the
          skylines agree - a fit worse than 0.06 of the height: the place or the scene does not match). --poses: phototex.py's
          fits (place, heading, camera height and tilt against the laser scan's skyline) where they are good (--agree 0.35,
          --miss 6), and only the pictures with one (--any 1: all, the others at their own place snapped to a lane and the
          skyline fit against the game's)
"""
import io, json, math, os, random, re, subprocess, sys, time, urllib.parse, urllib.request

import numpy as np
from PIL import Image, ImageDraw

GAME = r'D:\SteamLibrary\steamapps\common\Grand Theft Auto V Enhanced'
GTACTL = os.path.join(os.path.dirname(os.path.abspath(__file__)), '..', 'gtactl.py')
NAIP = 'https://imagery.nationalmap.gov/arcgis/rest/services/USGSNAIPPlus/ImageServer/exportImage'
COMMONS = 'https://commons.wikimedia.org/w/api.php'
AGENT = {'User-Agent': 'GTAVE-ModLoader-worldgen/1.0 (comparison with real places; contact via the project)'}
FOV = 60.0  # the game camera's vertical field of view for the views from above
EYE_FOV = 50.0


def options(argv):
    o = {}
    for i in range(0, len(argv) - 1, 2):
        if not argv[i].startswith('--'):
            sys.exit(__doc__)
        o[argv[i][2:]] = argv[i + 1]
    return o


class Land:
    def __init__(self, name):
        with open(os.path.join(GAME, 'ModLoader', 'mods', name, 'land.json'), encoding='utf-8') as f:
            j = json.load(f)
        self.name = name
        self.lat0, self.lon0, self.x0, self.y0, self.kx, self.ky = j['projection']
        self.world = j['world']

    def latlon(self, x, y):
        return self.lat0 + (y - self.y0) / self.ky, self.lon0 + (x - self.x0) / self.kx

    def game(self, lat, lon):
        return self.x0 + (lon - self.lon0) * self.kx, self.y0 + (lat - self.lat0) * self.ky


def fetch(url, timeout=60, tries=3):
    for k in range(tries):
        try:
            with urllib.request.urlopen(urllib.request.Request(url, headers=AGENT), timeout=timeout) as r:
                return r.read()
        except Exception as e:
            if k == tries - 1:
                raise
            time.sleep(2 + 3 * k)


def gtactl(*steps):
    r = subprocess.run([sys.executable, '-I', GTACTL, *steps], capture_output=True, text=True)
    if r.returncode:
        raise RuntimeError(f'gtactl {" ".join(steps)}: {r.stdout} {r.stderr}')
    return r.stdout


def hud_off():
    """The radar and the HUD hidden (DISPLAY_RADAR, DISPLAY_HUD: they stay so) - the pictures have none. First the world
    there: no loading screen (GET_IS_LOADING_SCREEN_ACTIVE), the screen not faded out (IS_SCREEN_FADED_OUT) - a comparison
    started as story mode was still loading had taken the loading screen's art for the first place's views."""
    end = time.time() + 600
    while time.time() < end:
        loading = gtactl('raw:native 0x10D0A8F259E93EC9').split()
        faded = gtactl('raw:native 0xB16FCE9DDC7BA182').split()
        if len(loading) > 1 and len(faded) > 1 and loading[-6:][1] == '0' and faded[-6:][1] == '0':
            break
        time.sleep(5)
    # (clear weather, the roads dry by the first place: a game started in the rain had mirror-like puddles in every street,
    # each one a difference to the reviewers)
    gtactl('raw:weather CLEAR')
    gtactl('wait:10')
    gtactl('raw:native 0xA0EBB943C300E693 0', 'raw:native 0xA6294919E56FF02A 0')


def road_near(x, y, z, compass, reach=12.0):
    """The street a car's camera was on: the nearest road node (GET_CLOSEST_VEHICLE_NODE) within `reach` m, then 1.8 m
    to the right of the way the picture faces (the lane it drove in); none: where it was."""
    out = gtactl(f'raw:native 0x240A18690AE96513 {x:.1f} {y:.1f} {z:.1f} out 1 3.0 0')
    m = re.search(r'\| out \S+ \S+ (\S+) (\S+) (\S+)', out)
    if not out.startswith('ok 1') or not m:
        return x, y
    nx, ny = float(m.group(1)), float(m.group(2))
    if math.hypot(nx - x, ny - y) > reach:
        return x, y
    a = math.radians(compass)
    return nx + 1.8 * math.cos(a), ny - 1.8 * math.sin(a)


def road_along(roads, x, y, compass, reach=15.0, carriageway=False):
    """The street a car's camera was on, from the land's world.json roads: the nearest stretch within `reach` m that runs
    within 35 degrees of the way the picture faces (either way along a two-way road, its own way along a one-way one), the
    camera in the middle of a lane it could drive in - a two-way road's on the right of its middle - the one nearest where
    the picture says it was; none: None. `carriageway`: only when that place is on the road or by its edge (2.5 m; farther
    out someone walking took it)."""
    hx, hy = math.sin(math.radians(compass)), math.cos(math.radians(compass))
    best = None
    for pts, lane, fwd, bwd in roads:
        a, b = pts[:-1], pts[1:]
        d = b - a
        length = np.hypot(d[:, 0], d[:, 1])
        ok = length > 0.5
        u = d[ok] / length[ok, None]
        aa, ll = a[ok], length[ok]
        dot = u[:, 0] * hx + u[:, 1] * hy
        along = np.abs(dot) >= math.cos(math.radians(35))
        if fwd > 0 and bwd <= 0:
            along &= dot > 0
        elif bwd > 0 and fwd <= 0:
            along &= dot < 0
        if not along.any():
            continue
        u, aa, ll = u[along], aa[along], ll[along]
        t = np.clip((x - aa[:, 0]) * u[:, 0] + (y - aa[:, 1]) * u[:, 1], 0, ll)
        qx, qy = aa[:, 0] + u[:, 0] * t, aa[:, 1] + u[:, 1] * t
        dist = np.hypot(qx - x, qy - y)
        k = int(np.argmin(dist))
        if dist[k] < reach and (best is None or dist[k] < best[0]):
            best = (float(dist[k]), float(qx[k]), float(qy[k]), float(u[k, 0] * hx + u[k, 1] * hy) > 0, lane, fwd, bwd)
    if best is None:
        return None
    _, qx, qy, same, lane, fwd, bwd = best
    across = (x - qx) * hy - (y - qy) * hx  # (metres right of the way it faces: (hy, -hx))
    half = max(fwd + bwd, 1) * lane / 2
    if carriageway and abs(across) > half + 2.5:
        return None
    if fwd > 0 and bwd > 0:
        n = fwd if same else bwd
        off = (min(max(int(across // lane), 0), n - 1) + 0.5) * lane
    else:
        n = max(fwd, bwd, 1)
        off = -half + (min(max(int((across + half) // lane), 0), n - 1) + 0.5) * lane
    return qx + off * hy, qy - off * hx


def land_roads(land):
    """The land's roads from its world.json (D:\\GTAV-UE5\\world\\<land>\\world.json; Miami's wbmia: miami): each one's points
    (game x, y), lane width and whether it goes both ways; none when the world is not there (the sectors' are removed once
    built)."""
    folder = 'miami' if land.name == 'wbmia' else land.name
    path = os.path.join(r'D:\GTAV-UE5\world', folder, 'world.json')
    if not os.path.exists(path):
        return []
    with open(path, encoding='utf-8') as f:
        w = json.load(f)
    out = []
    for r in w.get('roads', []):
        if r.get('kind') in ('footway', 'path', 'cycleway', 'steps', 'pedestrian', 'track') or len(r.get('points', [])) < 2:
            continue
        out.append((np.array(r['points'], np.float64)[:, :2], float(r.get('laneWidth', 3.5)), int(r.get('forward', 1)), int(r.get('backward', 1))))
    return out


_dems = {}


def dem_at(land, x, y):
    """The land's measured ground (D:\GTAV-UE5\world\<land>\ground.bin, dem.py's: 'WBGR' version 1, x0 y0 cell nx ny, int16 cm
    rows south to north) at a point; None where it is not known or there is none."""
    if land.name not in _dems:
        folder = 'miami' if land.name == 'wbmia' else land.name
        path = os.path.join(r'D:\GTAV-UE5\world', folder, 'ground.bin')
        _dems[land.name] = None
        if os.path.exists(path):
            with open(path, 'rb') as f:
                head = f.read(28)
                if head[:4] == b'WBGR':
                    import struct
                    _, gx, gy, gc, nx, ny = struct.unpack('<Ifff2I', head[4:])
                    _dems[land.name] = (gx, gy, gc, np.frombuffer(f.read(2 * nx * ny), '<i2').reshape(ny, nx))
    d = _dems[land.name]
    if d is None:
        return None
    gx, gy, gc, a = d
    i, j = int((x - gx) / gc), int((y - gy) / gc)
    if not (0 <= j < a.shape[0] and 0 <= i < a.shape[1]) or a[j, i] == -32768:
        return None
    return a[j, i] / 100.0


def ground_z(x, y, land=None):
    """The game's ground under a point: the first surface down from 3 m over the measured ground where that is known - from
    high up it was a roof now and then, the camera on it (a campus walk between two buildings) -, else from 1000 m."""
    top = 1000.0
    if land is not None and (d := dem_at(land, x, y)) is not None:
        top = d + 3.0
    out = gtactl(f'raw:native 0xC906A7DAB05C8D2B {x:.1f} {y:.1f} {top:.1f} out 0 0')
    m = re.search(r'\| out \S+ (\S+)', out)
    return float(m.group(1)) if m and out.startswith('ok 1') else None


def aerial_picture(land, x0, y0, x1, y1, width, height):
    """NAIP over the game box (its pixels even in latitude and longitude, as the game's x and y are)."""
    s, w = land.latlon(x0, y0)
    n, e = land.latlon(x1, y1)
    q = urllib.parse.urlencode({'bbox': f'{w},{s},{e},{n}', 'bboxSR': 4326, 'imageSR': 4326, 'size': f'{width},{height}', 'format': 'jpg',
                                'f': 'image'})
    return Image.open(io.BytesIO(fetch(f'{NAIP}?{q}'))).convert('RGB')


def shares(image):
    """A picture's share of cover by colour (rough, the same rule for both pictures): tree-dark green, light green,
    bare or dry (tan, sand), grey (paving, roofs), dark (water, shadow), and each one's mean colour."""
    a = np.asarray(image.convert('RGB'), np.float32)
    r, g, b = a[..., 0], a[..., 1], a[..., 2]
    v = a.max(axis=-1)
    sat = (v - a.min(axis=-1)) / np.maximum(v, 1)
    dark = v < 45
    green = (g >= r) & (g >= b) & ~dark
    trees = green & (v < 100)
    grass = green & ~trees
    tan = ~dark & ~green & (r > b + 12) & (sat > 0.15)
    grey = ~dark & ~green & ~tan
    out = {}
    for name, m in (('trees', trees), ('grass', grass), ('bare', tan), ('grey', grey), ('dark', dark)):
        share = float(m.mean())
        mean = a[m].mean(axis=0) if m.any() else np.zeros(3)
        out[name] = (share, tuple(int(c) for c in mean))
    return out


def label(image, text):
    d = ImageDraw.Draw(image)
    d.rectangle((0, 0, 8 + 7 * len(text), 18), fill=(0, 0, 0))
    d.text((4, 3), text, fill=(255, 255, 255))
    return image


def aerial(o):
    land = Land(o['land'])
    out = o['out']
    os.makedirs(out, exist_ok=True)
    size = float(o.get('size', 240))
    width_m = size * 16 / 9
    if 'at' in o:
        points = [tuple(float(v) for v in p.split(',')) for p in o['at'].split(';')]
    else:
        rnd = random.Random(land.name)
        x0, y0, x1, y1 = land.world
        points = [(rnd.uniform(x0 + width_m, x1 - width_m), rnd.uniform(y0 + size, y1 - size)) for _ in range(int(o.get('count', 6)))]
    rows = []
    hud_off()
    for cx, cy in points:
        gz = ground_z(cx, cy)
        if gz is None:
            # (no collision streamed there yet: the camera there first)
            gtactl(f'raw:cam {cx:.1f} {cy:.1f} 300 0 -89.9 {FOV}', 'wait:6')
            gz = ground_z(cx, cy) or 0.0
        height = size / 2 / math.tan(math.radians(FOV / 2))
        shot = os.path.join(out, f'game_{cx:.0f}_{cy:.0f}.png')
        gtactl(f'raw:cam {cx:.1f} {cy:.1f} {gz + height:.1f} 0 -89.9 {FOV}', 'wait:8', f'shot:{shot}')
        game = Image.open(shot).convert('RGB')
        gw, gh = game.size
        real = aerial_picture(land, cx - width_m / 2, cy - size / 2, cx + width_m / 2, cy + size / 2, gw, gh)
        real.save(os.path.join(out, f'naip_{cx:.0f}_{cy:.0f}.jpg'), quality=92)
        a, g = shares(real), shares(game)
        sheet = Image.new('RGB', (gw, gh * 2 + 8), (255, 255, 255))
        sheet.paste(label(real.copy(), f'NAIP {land.latlon(cx, cy)[0]:.5f},{land.latlon(cx, cy)[1]:.5f}'), (0, 0))
        sheet.paste(label(game.copy(), f'game {land.name} {cx:.0f},{cy:.0f}'), (0, gh + 8))
        sheet.save(os.path.join(out, f'aerial_{cx:.0f}_{cy:.0f}.png'))
        rows.append([f'{cx:.0f}', f'{cy:.0f}'] + [f'{a[k][0]:.2f}/{g[k][0]:.2f}' for k in a] + [f'{a[k][1]}/{g[k][1]}' for k in a])
        print(f'{cx:.0f},{cy:.0f}: ' + ', '.join(f'{k} {a[k][0]:.2f} real {g[k][0]:.2f} game' for k in a), flush=True)
    with open(os.path.join(out, 'aerial.tsv'), 'a', encoding='utf-8') as f:
        f.write('\t'.join(['x', 'y', 'trees', 'grass', 'bare', 'grey', 'dark', 'trees rgb', 'grass rgb', 'bare rgb', 'grey rgb', 'dark rgb']) + '\n')
        for r in rows:
            f.write('\t'.join(r) + '\n')


def commons_photos(land, limit):
    """Commons' geotagged photographs in the land's box (searches 10 km round points over it; JPEGs only, not the
    aerial tiles or the space station's views of a whole region)."""
    x0, y0, x1, y1 = land.world
    seen, found = set(), []
    for gy in np.arange(y0 + 5000, y1 + 5000, 10000):
        for gx in np.arange(x0 + 5000, x1 + 5000, 10000):
            lat, lon = land.latlon(gx, gy)
            q = urllib.parse.urlencode({'action': 'query', 'list': 'geosearch', 'gscoord': f'{lat}|{lon}', 'gsradius': 10000, 'gsnamespace': 6,
                                        'gslimit': 500, 'format': 'json'})
            for g in json.loads(fetch(f'{COMMONS}?{q}'))['query']['geosearch']:
                t = g['title']
                if t in seen or not re.search(r'\.jpe?g$', t, re.I) or re.search(r'ISS\d|View of Earth|\bmap\b', t, re.I):
                    continue
                x, y = land.game(g['lat'], g['lon'])
                if not (x0 <= x <= x1 and y0 <= y <= y1):
                    continue
                seen.add(t)
                found.append((t, g['lat'], g['lon']))
    random.Random(land.name).shuffle(found)
    return found[:limit]


def photo_info(title):
    q = urllib.parse.urlencode({'action': 'query', 'titles': title, 'prop': 'imageinfo', 'iiprop': 'url|extmetadata|metadata', 'iiurlwidth': 1280,
                                'format': 'json'})
    page = next(iter(json.loads(fetch(f'{COMMONS}?{q}'))['query']['pages'].values()))
    info = page['imageinfo'][0]
    meta = {m['name']: m['value'] for m in info.get('metadata') or [] if isinstance(m, dict) and 'name' in m}
    ext = info.get('extmetadata', {})
    heading = None
    d = meta.get('GPSImgDirection')
    if d is not None:
        try:
            heading = float(d) if not isinstance(d, str) or '/' not in d else float(d.split('/')[0]) / float(d.split('/')[1])
        except ValueError:
            heading = None
    plain = lambda k: re.sub(r'<[^>]+>', '', ext.get(k, {}).get('value', '')).strip()
    return {'url': info.get('thumburl') or info['url'], 'page': info.get('descriptionurl', ''), 'licence': plain('LicenseShortName'), 'author': plain('Artist'),
            'date': plain('DateTimeOriginal'), 'heading': heading}


def photos(o):
    land = Land(o['land'])
    out = o['out']
    os.makedirs(out, exist_ok=True)
    listed = []
    for k, (title, lat, lon) in enumerate(commons_photos(land, int(o.get('limit', 12)))):
        info = photo_info(title)
        real = Image.open(io.BytesIO(fetch(info['url']))).convert('RGB')
        real.thumbnail((960, 720))
        x, y = land.game(lat, lon)
        gz = ground_z(x, y, land)
        if gz is None:
            gtactl(f'raw:cam {x:.1f} {y:.1f} 300 0 -89.9 {FOV}', 'wait:6')
            gz = ground_z(x, y) or 0.0
        # (compass degrees clockwise from north -> the game's heading, anticlockwise)
        compass = [info['heading']] if info['heading'] is not None else [0, 90, 180, 270]
        views = []
        for c in compass:
            shot = os.path.join(out, f'game_{k}_{int(c)}.png')
            gtactl(f'raw:cam {x:.1f} {y:.1f} {gz + 1.7:.1f} {(360 - c) % 360:.1f} 0 {EYE_FOV}', 'wait:6', f'shot:{shot}')
            views.append(label(Image.open(shot).convert('RGB'), f'game, facing {int(c)} deg'))
        vw, vh = 640, 360
        cols = 1 if len(views) == 1 else 2
        rows_ = (len(views) + cols - 1) // cols
        sheet = Image.new('RGB', (max(real.width, vw * cols), real.height + 8 + vh * rows_), (255, 255, 255))
        sheet.paste(label(real, f'{title[5:60]} ({info["licence"]})'), (0, 0))
        for i, v in enumerate(views):
            sheet.paste(v.resize((vw, vh)), ((i % cols) * vw, real.height + 8 + (i // cols) * vh))
        sheet.save(os.path.join(out, f'photo_{k}.png'))
        listed.append({'title': title, 'lat': lat, 'lon': lon, 'x': round(x, 1), 'y': round(y, 1), **info})
        print(f'{k}: {title} at {x:.0f},{y:.0f} ({"facing " + str(round(info["heading"])) if info["heading"] is not None else "four ways"})', flush=True)
    with open(os.path.join(out, 'photos.json'), 'w', encoding='utf-8') as f:
        json.dump(listed, f, ensure_ascii=False, indent=1)


def mapillary(o):
    """Street pictures from Mapillary (CC BY-SA 4.0; a download's metadata.csv: id, lat, lon, heading, date, pano,
    creator, ..., file) beside the game at each one's place and heading, the camera 2 m up (a car's or a walker's)."""
    land = Land(o['land'])
    out = o['out']
    os.makedirs(out, exist_ok=True)
    folder = os.path.dirname(o['metadata'])
    import csv
    with open(o['metadata'], encoding='utf-8') as f:
        rows = [r for r in csv.DictReader(f) if r.get('pano') != 'True']
    x0, y0, x1, y1 = land.world
    rows = [r for r in rows if x0 <= land.game(float(r['lat']), float(r['lon']))[0] <= x1 and y0 <= land.game(float(r['lat']), float(r['lon']))[1] <= y1]
    rnd = random.Random(int(o.get('seed', 1)))
    rnd.shuffle(rows)
    listed = []
    hud_off()
    roads = land_roads(land)
    for k, r in enumerate(rows[:int(o.get('limit', 12))]):
        lat, lon, compass = float(r['lat']), float(r['lon']), float(r['heading'])
        x, y = land.game(lat, lon)
        gtactl(f'raw:cam {x:.1f} {y:.1f} 60 0 -89.9 {FOV}', 'wait:4')
        gz = ground_z(x, y, land)
        if gz is None:
            gtactl('wait:4')
            gz = ground_z(x, y, land) or 1.0
        # (a phone's position is often some metres off: onto the road it was taken from when one is that near)
        snapped = road_along(roads, x, y, compass) if roads else None
        x, y = snapped if snapped else road_near(x, y, gz, compass)
        gz = ground_z(x, y, land) or gz
        shot = os.path.join(out, f'game_{r["id"]}.png')
        gtactl(f'raw:cam {x:.1f} {y:.1f} {gz + 2.0:.1f} {(360 - compass) % 360:.1f} 0 {EYE_FOV}', 'wait:7', f'shot:{shot}')
        real = Image.open(os.path.join(folder, r['file'])).convert('RGB')
        real.thumbnail((960, 720))
        game = Image.open(shot).convert('RGB')
        game = game.resize((960, round(960 * game.height / game.width)))
        sheet = Image.new('RGB', (max(real.width, game.width), real.height + 8 + game.height), (255, 255, 255))
        sheet.paste(label(real, f'Mapillary {r["id"]} {r["date"]} by {r["creator"]} (CC BY-SA 4.0), facing {compass:.0f}'), (0, 0))
        sheet.paste(label(game, f'game {land.name} {x:.0f},{y:.0f},{gz + 2:.1f} facing {compass:.0f}'), (0, real.height + 8))
        sheet.save(os.path.join(out, f'street_{k:02d}_{r["id"]}.png'))
        listed.append({'id': r['id'], 'lat': lat, 'lon': lon, 'heading': compass, 'x': round(x, 1), 'y': round(y, 1), 'date': r['date'], 'creator': r['creator'],
                       'page': r.get('page', '')})
        print(f'{k}: {r["id"]} at {x:.0f},{y:.0f} facing {compass:.0f}', flush=True)
    with open(os.path.join(out, 'street.json'), 'w', encoding='utf-8') as f:
        json.dump(listed, f, ensure_ascii=False, indent=1)


def pano_view(pano, heading, yaw, pitch, vfov, w, h):
    """A perspective view (w x h, `vfov` degrees tall) out of an equirectangular panorama whose middle column faces
    `heading` (compass degrees): towards compass `yaw`, `pitch` degrees up; bilinear."""
    H, W = pano.shape[:2]
    f = (h / 2) / math.tan(math.radians(vfov) / 2)
    X, Y = np.meshgrid((np.arange(w) - w / 2 + 0.5) / f, (h / 2 - np.arange(h) - 0.5) / f)
    cp, sp = math.cos(math.radians(pitch)), math.sin(math.radians(pitch))
    Y2, Z2 = Y * cp + sp, -Y * sp + cp
    az = np.degrees(np.arctan2(X, Z2)) + yaw
    el = np.degrees(np.arctan2(Y2, np.hypot(X, Z2)))
    px = (((az - heading) / 360 + 0.5) % 1.0) * W - 0.5
    py = np.clip((0.5 - el / 180) * H - 0.5, 0, H - 1.001)
    x0, y0 = np.floor(px).astype(np.int64), np.floor(py).astype(np.int64)
    fx, fy = (px - x0)[..., None], (py - y0)[..., None]
    x0 %= W
    x1 = (x0 + 1) % W
    y1 = np.minimum(y0 + 1, H - 1)
    return pano[y0, x0] * (1 - fx) * (1 - fy) + pano[y0, x1] * fx * (1 - fy) + pano[y1, x0] * (1 - fx) * fy + pano[y1, x1] * fx * fy


def skyline(img):
    """Per column, how far down the sky reaches (a share of the height): the first run of four pixels from the top that
    are not sky (wires and gaps in the leaves passed over); sky: bluish and bright, or near white."""
    a = np.asarray(img, np.float32)
    r, g, b = a[..., 0], a[..., 1], a[..., 2]
    mx, mn = a.max(-1), a.min(-1)
    sat = (mx - mn) / np.maximum(mx, 1)
    ground = ~(((b >= r) & (b >= g - 8) & (mx > 110)) | ((mx > 190) & (sat < 0.12)))
    run = ground[:-3] & ground[1:-2] & ground[2:-1] & ground[3:]
    h = a.shape[0]
    return np.where(run.any(axis=0), run.argmax(axis=0), h) / h


def fit_view(pano, heading, yaw, game_small):
    """The panorama's yaw and pitch (within 10 and 3 degrees of `yaw`, 0) whose skyline fits the game's best, and how far
    apart the skylines then are (the mean over the columns where either has one, a share of the height). The picture's
    own heading unless a fit clearly beats it - a quarter closer, not at the search's edge: a skyline of trees alone (or
    a place that does not match) fits anywhere about as badly, and the fit then wandered to the edges."""
    target = skyline(game_small)
    h, w = game_small.shape[:2]

    def score_of(dy, pitch):
        sky = skyline(pano_view(pano, heading, yaw + dy, pitch, EYE_FOV, w, h))
        both = (sky < 0.98) | (target < 0.98)
        return float(np.abs(sky - target)[both].mean()) if both.sum() >= w // 4 else 1e9

    plain = score_of(0, 0)
    best = (plain, 0.0, 0.0)
    for pitch in (-3, 0, 3):
        for dy in range(-10, 11):
            if (dy, pitch) != (0, 0):
                score = score_of(dy, pitch)
                if score < best[0]:
                    best = (score, float(dy), float(pitch))
    score, dy, pitch = best
    for fine in (-0.5, 0.5):
        if (f := score_of(dy + fine, pitch)) < score:
            score, dy = f, dy + fine
    if abs(dy) >= 9.5 or score > 0.75 * plain:
        return 0.0, 0.0, plain
    return dy, pitch, score


def pano(o):
    """Mapillary panoramas (CC BY-SA 4.0) beside the game's exact view at their places (see the usage)."""
    land = Land(o['land'])
    out = o['out']
    os.makedirs(out, exist_ok=True)
    import csv
    rows = []
    for meta in o['metadata'].split(','):
        with open(meta, encoding='utf-8') as f:
            for r in csv.DictReader(f):
                if r.get('pano') == 'True' and os.path.exists(os.path.join(os.path.dirname(meta), r['file'])):
                    r['path'] = os.path.join(os.path.dirname(meta), r['file'])
                    rows.append(r)
    x0, y0, x1, y1 = land.world
    rows = [r for r in rows if x0 + 50 <= land.game(float(r['lat']), float(r['lon']))[0] <= x1 - 50 and
            y0 + 50 <= land.game(float(r['lat']), float(r['lon']))[1] <= y1 - 50]
    rnd = random.Random(int(o.get('seed', 1)))
    rnd.shuffle(rows)
    # (--ids a,b,...: only those pictures - a check of a few places, the same as a full comparison took them)
    if 'ids' in o:
        wanted = o['ids'].split(',')
        rows = [r for r in rows if r['id'] in wanted]
    fits = {}
    if 'poses' in o:
        with open(o['poses'], encoding='utf-8') as f:
            fits = {p['id']: p for p in json.load(f) if p.get('ok') and p.get('agree', 0) >= float(o.get('agree', 0.35)) and
                    p.get('miss', 99) <= float(o.get('miss', 6))}
        # (only the pictures whose place and heading the fit found: the others' places are the GPS's - metres off, now and
        # then on another stretch of road, their headings half a turn off - and what they were compared with was not what
        # they show; --any 1: all of them)
        if o.get('any') != '1':
            rows = [r for r in rows if r['id'] in fits]
    # (spread out: each at least --spacing m from those taken)
    spacing, chosen = float(o.get('spacing', 80)), []
    for r in rows:
        x, y = land.game(float(r['lat']), float(r['lon']))
        if all(math.hypot(x - cx, y - cy) >= spacing for cx, cy, _ in chosen):
            chosen.append((x, y, r))
        if len(chosen) >= int(o.get('limit', 12)):
            break
    views = [float(v) for v in o.get('views', '0,90,-90').split(',')]
    listed = []
    hud_off()
    roads = land_roads(land)
    # (the player's ped: PLAYER_PED_ID)
    reply = gtactl('raw:native 0xD80958FC74E988A6')
    m = re.match(r'ok (-?\d+)', reply)
    ped = int(m.group(1)) if m else None
    for k, (x, y, r) in enumerate(chosen):
        compass = float(r['heading'])
        # (no traffic or people while the pictures are taken - SET_VEHICLE_DENSITY_ and SET_PED_DENSITY_MULTIPLIER_THIS_FRAME 0:
        # they are not the place, a car had stopped on the camera; the parked cars stay. The phone hidden (SET_MOBILE_PHONE_SCALE 0): a call came up over a view. Each place anew: an
        # "every" lasts at most 600 s)
        gtactl('raw:every 120 0x245A6883D966D537 0.0', 'raw:every 120 0x95E3D6257B166CF2 0.0', 'raw:every 120 0xCBDD322A73D6D932 0.0')
        gtactl(f'raw:cam {x:.1f} {y:.1f} 60 0 -89.9 {FOV}', 'wait:4')
        gz = ground_z(x, y, land)
        if gz is None:
            gtactl('wait:4')
            gz = ground_z(x, y, land) or 1.0
        # (the panorama's place is a few metres off as often as not - a camera on a car put on the wrong side of the middle
        # line: onto the lane it drove in when it is on the road; phototex.py's fit where there is a good one)
        fit = fits.get(r['id'])
        cam_up = 2.0
        if fit:
            x, y, compass = fit['fx'], fit['fy'], fit['fh']
            gz = ground_z(x, y, land) or gz
            cam_up = max(1.2, fit['zc'] - gz)
        else:
            snapped = road_along(roads, x, y, compass, 12.0, carriageway=True) if roads else None
            if snapped:
                x, y = snapped
                gz = ground_z(x, y, land) or gz
        # (the parked cars: the game makes them only near the player and out of the camera's sight - the player there,
        # unseen (SET_ENTITY_VISIBLE 0), the camera looking up at the sky a while; then only what stands on the camera's
        # spot cleared (CLEAR_AREA 3 m, objects kept): the driveways' and the lots' cars stay)
        gtactl(f'raw:tp {x:.1f} {y:.1f} {gz + 0.5:.1f}', 'wait:1')
        if ped is not None:
            gtactl(f'raw:native 0xEA1C610A04DB6BBB {ped} 0 0')
        gtactl(f'raw:cam {x:.1f} {y:.1f} {gz + cam_up:.1f} 0 89 {EYE_FOV}', 'wait:6')
        gtactl(f'raw:native 0xA56F01F3765B93A0 {x:.1f} {y:.1f} {gz:.1f} 3.0 1 0 1 0')  # (CLEAR_AREA round the camera, objects kept)
        picture = np.asarray(Image.open(r['path']).convert('RGB'), np.float32)
        small = np.asarray(Image.fromarray(picture.astype(np.uint8)).resize((1024, 512)), np.float32)
        last = None
        for view in views:
            yaw = (compass + view) % 360
            shot = os.path.join(out, f'game_{r["id"]}_{view:+.0f}.png')
            # (taken again - twice at most, 6 s later - when it is black or the same as the last view's: the camera had not
            # turned yet or the screen was still fading in, the game lagging under the batch's load)
            for attempt in range(3):
                gtactl(f'raw:cam {x:.1f} {y:.1f} {gz + cam_up:.2f} {(360 - yaw) % 360:.1f} 0 {EYE_FOV}', 'wait:6', f'shot:{shot}')
                game = Image.open(shot).convert('RGB').resize((960, 540))
                grey = np.asarray(game.resize((96, 54)).convert('L'), np.float32)
                if attempt == 2 or (grey.mean() > 12 and (last is None or np.abs(grey - last).mean() > 4)):
                    break
                print(f'{k}: {r["id"]} view {view:+.0f}: a stale or black frame, taken again', flush=True)
                gtactl('wait:6')
            last = grey
            if fit:
                # (the fit's tilt at this view: the picture's elevations are off by it - the view's pitch)
                rel = math.radians(view)
                dy, pitch = 0.0, fit['tilt'][0] * math.cos(rel) + fit['tilt'][1] * math.sin(rel)
                score = float(np.abs(skyline(np.asarray(pano_view(small, compass, yaw, pitch, EYE_FOV, 320, 180), np.float32)) -
                                     skyline(np.asarray(game.resize((320, 180)), np.float32))).mean())
            else:
                dy, pitch, score = fit_view(small, compass, yaw, np.asarray(game.resize((320, 180)), np.float32))
            real = Image.fromarray(np.clip(pano_view(picture, compass, yaw + dy, pitch, EYE_FOV, 960, 540), 0, 255).astype(np.uint8))
            sheet = Image.new('RGB', (960, 540 * 2 + 8), (255, 255, 255))
            sheet.paste(label(real, f'Mapillary {r["id"]} {r["date"]} by {r["creator"]} (CC BY-SA 4.0), facing {yaw + dy:.0f} '
                                    f'(fit {dy:+.1f}, pitch {pitch:+.0f})'), (0, 0))
            sheet.paste(label(game, f'game {land.name} {x:.0f},{y:.0f},{gz + cam_up:.1f} facing {yaw:.0f}'), (0, 548))
            name = f'pano_{k:02d}_{r["id"]}_{view:+.0f}.png'
            sheet.save(os.path.join(out, name))
            listed.append({'sheet': name, 'id': r['id'], 'lat': float(r['lat']), 'lon': float(r['lon']), 'x': round(x, 1), 'y': round(y, 1),
                           'heading': round(yaw, 1), 'fit': dy, 'pitch': pitch, 'skyline': round(score, 3), 'date': r['date'],
                           'creator': r['creator'], 'page': r.get('page', '')})
            print(f'{k}: {r["id"]} view {view:+.0f} at {x:.0f},{y:.0f}: fit {dy:+.1f}, pitch {pitch:+.0f}, skylines {score:.3f}', flush=True)
    gtactl('raw:every off')
    with open(os.path.join(out, 'pano.json'), 'w', encoding='utf-8') as f:
        json.dump(listed, f, ensure_ascii=False, indent=1)


def main():
    commands = {'aerial': aerial, 'photos': photos, 'mapillary': mapillary, 'pano': pano}
    if len(sys.argv) < 2 or sys.argv[1] not in commands:
        sys.exit(__doc__)
    o = options(sys.argv[2:])
    if 'land' not in o or 'out' not in o or (sys.argv[1] in ('mapillary', 'pano') and 'metadata' not in o):
        sys.exit(__doc__)
    commands[sys.argv[1]](o)


if __name__ == '__main__':
    main()
