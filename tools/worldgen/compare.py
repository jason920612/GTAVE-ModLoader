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


def ground_z(x, y):
    out = gtactl(f'raw:native 0xC906A7DAB05C8D2B {x:.1f} {y:.1f} 1000.0 out 0 0')
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
        gz = ground_z(x, y)
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
    for k, r in enumerate(rows[:int(o.get('limit', 12))]):
        lat, lon, compass = float(r['lat']), float(r['lon']), float(r['heading'])
        x, y = land.game(lat, lon)
        gtactl(f'raw:cam {x:.1f} {y:.1f} 60 0 -89.9 {FOV}', 'wait:4')
        gz = ground_z(x, y)
        if gz is None:
            gtactl('wait:4')
            gz = ground_z(x, y) or 1.0
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


def main():
    commands = {'aerial': aerial, 'photos': photos, 'mapillary': mapillary}
    if len(sys.argv) < 2 or sys.argv[1] not in commands:
        sys.exit(__doc__)
    o = options(sys.argv[2:])
    if 'land' not in o or 'out' not in o or (sys.argv[1] == 'mapillary' and 'metadata' not in o):
        sys.exit(__doc__)
    commands[sys.argv[1]](o)


if __name__ == '__main__':
    main()
