"""A tour of new land for checking it against the real place (research/phase0.md §61.20): pictures from the running game
(through the test pipe, tools/gtactl.py) next to what the land really looks like.

usage (a Python with numpy; the game running with test control, the world builder's pack in):
  worldtour.py top    --world <world.json> --aerial <aerial.py tiles> --area x0,y0,x1,y1 --out <dir> [--height 150] [--step 220]
  worldtour.py street --world <world.json> --area x0,y0,x1,y1 --out <dir> [--every 150] [--max 40]

top: the area in a grid of straight-down views, each beside the aerial photo of the same ground (north up), as
<out>/top_<x>_<y>.png (game left, photo right).
street: along the roads in the area, a view at eye height down the road every `every` metres, <out>/street_<n>.png, and
street.csv (where each was taken).
The views are the game's own frames (shot), the camera the pipe's scripted one at noon, clear sky.
"""
import json, math, os, struct, sys, time, zlib

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from gtactl import Pipe  # noqa: E402

def options(argv):
    o = {}
    i = 0
    while i < len(argv):
        if argv[i].startswith('--') and i + 1 < len(argv):
            o[argv[i][2:]] = argv[i + 1]
            i += 2
        else:
            i += 1
    return o

def read_png(path):
    import numpy as np
    d = open(path, 'rb').read()
    i, idat, w, h, ct = 8, b'', 0, 0, 2
    while i < len(d):
        n = struct.unpack('>I', d[i:i + 4])[0]
        t, c = d[i + 4:i + 8], d[i + 8:i + 8 + n]
        if t == b'IHDR':
            w, h, _, ct = struct.unpack('>IIBB', c[:10])
        elif t == b'IDAT':
            idat += c
        i += 12 + n
    ch = 4 if ct == 6 else 3
    raw = zlib.decompress(idat)
    stride = w * ch + 1
    out = np.zeros((h, w * ch), np.uint8)
    prev = np.zeros(w * ch, np.int32)
    for y in range(h):
        f = raw[y * stride]
        line = np.frombuffer(raw[y * stride + 1:(y + 1) * stride], np.uint8).astype(np.int32)
        if f == 1:
            for x in range(ch, w * ch):
                line[x] = (line[x] + line[x - ch]) & 255
        elif f == 2:
            line = (line + prev) & 255
        elif f == 3:
            for x in range(w * ch):
                left = line[x - ch] if x >= ch else 0
                line[x] = (line[x] + (left + prev[x]) // 2) & 255
        elif f == 4:
            for x in range(w * ch):
                a = line[x - ch] if x >= ch else 0
                b = prev[x]
                c2 = prev[x - ch] if x >= ch else 0
                p = a + b - c2
                pa, pb, pc = abs(p - a), abs(p - b), abs(p - c2)
                line[x] = (line[x] + (a if pa <= pb and pa <= pc else b if pb <= pc else c2)) & 255
        out[y] = line
        prev = line
    return out.reshape(h, w, ch)[:, :, :3]

def write_png(path, img):
    h, w, _ = img.shape
    raw = b''.join(b'\x00' + img[r].tobytes() for r in range(h))
    def chunk(t, data):
        c = struct.pack('>I', len(data)) + t + data
        return c + struct.pack('>I', zlib.crc32(t + data) & 0xffffffff)
    open(path, 'wb').write(b'\x89PNG\r\n\x1a\n' + chunk(b'IHDR', struct.pack('>IIBBBBB', w, h, 8, 2, 0, 0, 0)) +
                           chunk(b'IDAT', zlib.compress(raw, 6)) + chunk(b'IEND', b''))

def resize(img, w, h):
    import numpy as np
    ys = (np.arange(h) * img.shape[0] / h).astype(int)
    xs = (np.arange(w) * img.shape[1] / w).astype(int)
    return img[ys][:, xs]

def aerial_crop(folder, x0, y0, x1, y1, w, h):
    """The aerial tiles (aerial.py: a_<i>_<j>.bin, 0.25 m, rows south to north) over the box, north up, w x h."""
    import numpy as np
    out = np.zeros((h, w, 3), np.uint8)
    xs = x0 + (np.arange(w) + 0.5) * (x1 - x0) / w
    ys = y1 - (np.arange(h) + 0.5) * (y1 - y0) / h
    gx, gy = np.meshgrid(xs, ys)
    cache = {}
    for (ti, tj) in {(int(math.floor(x / 256)), int(math.floor(y / 256))) for x in xs[::8] for y in ys[::8]}:
        path = os.path.join(folder, f'a_{ti}_{tj}.bin')
        if not os.path.exists(path):
            continue
        b = open(path, 'rb').read()
        tx, ty, cell, n, _, bands = struct.unpack_from('<fff3I', b, 8)
        cache[(ti, tj)] = (tx, ty, cell, n, np.frombuffer(b, np.uint8, n * n * 4, 32).reshape(n, n, 4))
    ti = np.floor(gx / 256).astype(int)
    tj = np.floor(gy / 256).astype(int)
    for (i, j), (tx, ty, cell, n, px) in cache.items():
        m = (ti == i) & (tj == j)
        if not m.any():
            continue
        ci = np.clip(((gx[m] - tx) / cell).astype(int), 0, n - 1)
        cj = np.clip(((gy[m] - ty) / cell).astype(int), 0, n - 1)
        out[m] = px[cj, ci, :3]
    return out

_held = [0.0, 0.0]

def hold_player(p, x, y, on=True):
    """The player kept out of harm while the camera tours: high over the spot (the world streams round it), frozen,
    invincible (a teleport's fall or a stray car would otherwise kill him, and he would wake far away). Let go
    (on=False): set down on the ground under the last spot first."""
    ped = p.send('native 0xD80958FC74E988A6').split()
    if len(ped) < 2:
        return
    handle = ped[1]
    if on:
        _held[:] = [x, y]
        p.send(f'native 0x3882114BDE571AD4 {handle} 1')   # SET_ENTITY_INVINCIBLE
        p.send(f'native 0x428CA6DBD1094446 {handle} 1')   # FREEZE_ENTITY_POSITION
        p.send(f'native 0x06843DA7060A026B {handle} {x:.1f} {y:.1f} 200.0 0 0 0 1')  # SET_ENTITY_COORDS
    else:
        hx, hy = _held
        reply = p.send(f'native 0xC906A7DAB05C8D2B {hx:.1f} {hy:.1f} 500.0 out 0 0')  # GET_GROUND_Z_FOR_3D_COORD
        try:
            z = float(reply.split('|')[1].split()[2]) if '|' in reply and int(reply.split()[1]) else None
        except (IndexError, ValueError):
            z = None
        if z is not None:
            p.send(f'native 0x06843DA7060A026B {handle} {hx:.1f} {hy:.1f} {z + 1.0:.1f} 0 0 0 1')
        p.send(f'native 0x428CA6DBD1094446 {handle} 0')
        p.send(f'native 0x3882114BDE571AD4 {handle} 0')

def top(o):
    import numpy as np
    x0, y0, x1, y1 = (float(v) for v in o['area'].split(','))
    height, step = float(o.get('height', 150)), float(o.get('step', 220))
    out = o['out']
    os.makedirs(out, exist_ok=True)
    p = Pipe(timeout=10)
    fov = 50.0
    # the ground a straight-down view at `height` covers (16:9; the game's field of view is vertical)
    gh = 2 * height * math.tan(math.radians(fov / 2))
    gw = gh * 16 / 9
    y = y0 + gh / 2
    while y < y1:
        x = x0 + gw / 2
        while x < x1:
            hold_player(p, x, y)
            p.send(f'cam {x:.1f} {y:.1f} {height:.1f} 0 -89.9 {fov}')
            time.sleep(6)
            shot = os.path.abspath(os.path.join(out, f'_shot.png'))
            if not p.send(f'shot {shot}').startswith('ok'):
                print('no shot at', x, y)
                x += step
                continue
            time.sleep(1.5)
            game = read_png(shot)
            photo = aerial_crop(o['aerial'], x - gw / 2, y - gh / 2, x + gw / 2, y + gh / 2, game.shape[1], game.shape[0])
            pair = np.concatenate([resize(game, 800, 450), resize(photo, 800, 450)], axis=1)
            write_png(os.path.join(out, f'top_{int(x)}_{int(y)}.png'), pair)
            print(f'top {x:.0f},{y:.0f}', flush=True)
            x += step
        y += step
    p.send('cam off')
    hold_player(p, 0, 0, False)

def street(o):
    x0, y0, x1, y1 = (float(v) for v in o['area'].split(','))
    every, most = float(o.get('every', 150)), int(o.get('max', 40))
    out = o['out']
    os.makedirs(out, exist_ok=True)
    with open(o['world'], encoding='utf-8') as f:
        w = json.load(f)
    spots = []
    for r in w['roads']:
        if r['kind'] in ('service', 'footway', 'track'):
            continue
        pts = r['points']
        run = every / 2
        for a, b in zip(pts, pts[1:]):
            l = math.hypot(b[0] - a[0], b[1] - a[1])
            while run < l:
                t = run / l
                x, y = a[0] + (b[0] - a[0]) * t, a[1] + (b[1] - a[1]) * t
                if x0 <= x <= x1 and y0 <= y <= y1:
                    # (the game's heading: 0 north, anticlockwise)
                    heading = math.degrees(math.atan2(-(b[0] - a[0]), b[1] - a[1]))
                    spots.append((x, y, heading, r['kind'], r.get('name', '')))
                run += every
            run -= l
    # spread over the area: every k-th
    if len(spots) > most:
        k = len(spots) / most
        spots = [spots[int(i * k)] for i in range(most)]
    p = Pipe(timeout=10)
    with open(os.path.join(out, 'street.csv'), 'w', encoding='utf-8') as csv:
        csv.write('n,x,y,heading,kind,name\n')
        for n, (x, y, heading, kind, name) in enumerate(spots):
            hold_player(p, x, y)
            time.sleep(4)
            # (eye height over the road: the ground's z from the native the pipe calls)
            reply = p.send(f'native 0xC906A7DAB05C8D2B {x:.1f} {y:.1f} 500.0 out 0 0')
            z = 2.0
            try:
                z = float(reply.split('|')[1].split()[2]) if '|' in reply else z
            except (IndexError, ValueError):
                pass
            p.send(f'cam {x:.1f} {y:.1f} {z + 1.7:.1f} {heading:.1f} 0 60')
            time.sleep(3)
            p.send('shot ' + os.path.abspath(os.path.join(out, f'street_{n:03d}.png')))
            csv.write(f'{n},{x:.1f},{y:.1f},{heading:.0f},{kind},"{name}"\n')
            print(f'street {n}: {x:.0f},{y:.0f} {kind} {name}', flush=True)
    p.send('cam off')
    hold_player(p, 0, 0, False)

if __name__ == '__main__':
    if len(sys.argv) < 2 or sys.argv[1] not in ('top', 'street'):
        sys.exit(__doc__)
    (top if sys.argv[1] == 'top' else street)(options(sys.argv[2:]))
