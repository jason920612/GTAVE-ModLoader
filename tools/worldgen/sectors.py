"""World builder sectors (research/phase0.md §62, §65): Florida cut into lands the size of the new land's place in the game's
world (x 4300..16032, y -3000..10404), each built as a pack of its own; the loader (game/sectors, game/seamless) keeps one
of them there and crosses into the next without a cut where the player goes over the edge of its own part.

One projection for the whole state (equirectangular from 24 N, 88 W, metres per degree at 27.5 N): a land is a translation
of it - its projection's x0, y0 -, so its neighbour's content is its own moved by a whole number of lands. A land's own
part (its core) is 8000 m by 10000 m of that plane, cell (column, row) = [column * 8000, (column + 1) * 8000) x [row * 10000,
(row + 1) * 10000) m from the origin - in the game at x 6166..14166, y -1298..8702; its box takes in a band of 1866 m (x)
and 1702 m (y) round it, the same real-world content as the neighbour's own part there (every random choice of the build
keyed to the real world's place: worldgen seeds.hpp). Named fl_<column>_<row>. For each cell:
its OpenStreetMap extract (worldgen osm) with the building footprints it lacks from Microsoft's US Building Footprints
(msbuildings.py; ODbL), its land cover (nlcd.py: NLCD's woods, wetlands, fields), its ground
(dem.py: 3DEP's measured heights, the lakes' and rivers' levels), its property rolls (parcels.py: the Department of
Revenue's parcels - vacant lots, mobile homes, storeys, the year built), what its aerial pictures show (photo.py, in the venv
of numpy, scipy, tifffile, zarr and Pillow: every tree, the ground's cover and colour - research/phase0.md §63), its build
(every building our own - hip-roofed houses, walls in the region's styles: the game's Los Santos models looked nothing
like Florida's -, the walls' colour and brick from the street pictures where there are any (photofacade.py, --mapillary),
power lines along the streets, parked cars, the trees and ground of the pictures, the game's plants on the ground,
navmeshes only where people go; every edge open: the land goes on into the neighbours), its population zones (zones.py,
into the land.json) and its pack (ModLoader\\mods\\<name>, or --lands; dlc.rpf.new when a pack is there already: the loader
takes it at the next start). The build's own files and the pictures' are removed once packed (--keep: kept).

usage: python -I tools/worldgen/sectors.py --cells c0,r0,c1,r1 | --list <cells.tsv> [--jobs n] [--prep n] [--skip-built] [--dry-run]
       [--keep] [--mapillary <metadata.csv>[,<metadata.csv>...]]
       python -I tools/worldgen/sectors.py --survey <cells.tsv>: the cells with land in them (worldgen survey on the state's
       extract, then the sea flood-filled away from the grid's edge: what is left - land, marshes, lakes - is built)
  --mapillary: street picture downloads (Mapillary, CC BY-SA 4.0): a sector with pictures in it gets its walls' colours
          from them
  --first c,r;c,r...: these cells before the others (the street pictures' places: compared first)
  --rebuild <cells.tsv>: these cells (column, row) before all others and built again though built (an older worldgen's
          lands: a dlc.rpf.new beside each pack, taken at the game's next start)
  --list: the survey's land cells (--survey: a tsv with column and row), built nearest --home first (column,row; by default
          Miami's cell)
  --names col,row=name;...  existing packs for some cells (not built again)
  --free <GB>: no cell started with less than this left on the drive its pack goes to or on the world folders' (120)
  --jobs: the builds at a time (worldgen build: the CPU's); --prep: the cells made ready at a time (the map, the land cover,
          the ground, the aerial and street pictures, the property rolls: mostly waiting for the services), ahead of the
          builds - at most --jobs + --prep + 2 cells under way
  --caches <folder>: the ground's tiles (dem\\) and the aerial pictures' answers (naip\\) kept there for every cell (a cell
          asks for its box and 3 km round it: half of it its neighbours' too), trimmed to --cachegb (60) oldest first
  --lands <folder>: a new land's pack goes there (<folder>\\<name>: a folder of the loader's landFolders, another drive's -
          the lands too many for the game's), one in ModLoader\\mods already stays there
Paths: --game, --worldgen, --keys, --pbf, --worlds (the world folders), --log as below by default.
"""
import collections, concurrent.futures, math, os, queue, shutil, subprocess, sys, threading, time

O = {
    'game': r'D:\SteamLibrary\steamapps\common\Grand Theft Auto V Enhanced',
    'worldgen': os.path.join(os.path.dirname(os.path.abspath(__file__)), '..', '..', 'build', 'Release', 'worldgen.exe'),
    'nlcd': os.path.join(os.path.dirname(os.path.abspath(__file__)), 'nlcd.py'),
    'dem': os.path.join(os.path.dirname(os.path.abspath(__file__)), 'dem.py'),
    'zones': os.path.join(os.path.dirname(os.path.abspath(__file__)), 'zones.py'),
    'photo': os.path.join(os.path.dirname(os.path.abspath(__file__)), 'photo.py'),
    'photofacade': os.path.join(os.path.dirname(os.path.abspath(__file__)), 'photofacade.py'),
    'msbuildings': os.path.join(os.path.dirname(os.path.abspath(__file__)), 'msbuildings.py'),
    'parcels': os.path.join(os.path.dirname(os.path.abspath(__file__)), 'parcels.py'),
    'parcelIndex': r'D:\GTAV-UE5\parcels\fl\index.json',  # (parcels.py fetch's: every county's parcels and property roll)
    'mscells': r'D:\GTAV-UE5\buildings\ms\cells',  # (msbuildings.py split's)
    'venv': r'D:\GTAV-UE5\venv\Scripts\python.exe',
    'keys': r'D:\GTAV-UE5\keys\ng.bin',
    'pbf': r'D:\GTAV-UE5\osm\pbf\florida.osm.pbf',
    'worlds': r'D:\GTAV-UE5\world',
    'names': '',
    'jobs': '1',
    'prep': '2',
    'caches': r'D:\GTAV-UE5\caches',
    'cachegb': '60',
    'log': r'D:\GTAV-UE5\world\florida\sectors.log',
}
# the state's projection (worldgen world::Projection: x = x0 + (lon - lon0) kx, y = y0 + (lat - lat0) ky - metres per degree
# at 27.5 N, WGS84), a land's core and its box in the game
LAT0, LON0, PHI = 24.0, -88.0, math.radians(27.5)
KY = 111132.92 - 559.82 * math.cos(2 * PHI) + 1.175 * math.cos(4 * PHI)
KX = 111412.84 * math.cos(PHI) - 93.5 * math.cos(3 * PHI) + 0.118 * math.cos(5 * PHI)
CORE_W, CORE_H = 8000, 10000
BOX = (4300.0, -3000.0, 16032.0, 10404.0)
BAND_X, BAND_Y = (BOX[2] - BOX[0] - CORE_W) / 2, (BOX[3] - BOX[1] - CORE_H) / 2
CORE = (BOX[0] + BAND_X, BOX[1] + BAND_Y, BOX[2] - BAND_X, BOX[3] - BAND_Y)
DLAT, DLON = CORE_H / KY, CORE_W / KX  # (the cells are a lattice of latitude and longitude: the survey counts on it)
HOME = (96, 19)  # (Miami's cell: built first)
FLAGS = ('skip-built', 'dry-run', 'keep')


def land(c, r):
    """Cell (c, r): its name, its projection (lat0, lon0, x0, y0, kx, ky), its box in degrees (south, west, north, east)."""
    x0, y0 = CORE[0] - c * CORE_W, CORE[1] - r * CORE_H  # (where the projection's origin is in the land's game coordinates)
    gx0, gy0, gx1, gy1 = BOX[0] - x0, BOX[1] - y0, BOX[2] - x0, BOX[3] - y0
    box = (LAT0 + gy0 / KY, LON0 + gx0 / KX, LAT0 + gy1 / KY, LON0 + gx1 / KX)
    return f'fl_{c:03d}_{r:03d}', (LAT0, LON0, x0, y0, KX, KY), box
g_print = threading.Lock()


def say(text):
    with g_print:
        line = time.strftime('%H:%M:%S ') + text
        print(line, flush=True)
        os.makedirs(os.path.dirname(O['log']), exist_ok=True)
        with open(O['log'], 'a', encoding='utf-8') as f:
            f.write(line + '\n')


def options(argv):
    flags = set()
    i = 0
    while i < len(argv):
        a = argv[i]
        if a[2:] in FLAGS:
            flags.add(a[2:])
            i += 1
        elif a.startswith('--') and i + 1 < len(argv):
            O[a[2:]] = argv[i + 1]
            i += 2
        else:
            sys.exit(f'unknown argument {a}')
    return flags


def run(cmd, log):
    with open(log, 'a', encoding='utf-8') as out:
        out.write('> ' + ' '.join(cmd) + '\n')
        out.flush()
        # (at idle priority, worldgen in background mode too: see background())
        return subprocess.run(cmd, stdout=out, stderr=subprocess.STDOUT, creationflags=getattr(subprocess, 'IDLE_PRIORITY_CLASS', 0),
                              env=dict(os.environ, WG_BACKGROUND='1')).returncode


def background():
    """This process in Windows' background mode (idle CPU, very low I/O and memory priority), its steps at idle priority:
    the game reads its files from the disk the batch writes, and played meanwhile at normal priority its streaming waited
    for the disk (frames of 100 ms to 2.7 s) and the batch's jobs took the CPU from its threads."""
    if os.name == 'nt':
        import ctypes
        kernel32 = ctypes.windll.kernel32
        kernel32.SetPriorityClass(kernel32.GetCurrentProcess(), 0x00100000)  # PROCESS_MODE_BACKGROUND_BEGIN


g_full = threading.Event()


def pack_of(name):
    """Where a land's pack is or goes: ModLoader\\mods\\<name> when it is there, else --lands\\<name> when given."""
    mods = os.path.join(O['game'], 'ModLoader', 'mods', name, 'dlc.rpf')
    if O.get('lands') and not os.path.exists(mods):
        return os.path.join(O['lands'], name, 'dlc.rpf')
    return mods


def trim(folder, limit):
    """A shared cache kept under `limit` bytes: the files used longest ago (their time: the last use) removed first."""
    files = []
    for root, _, names in os.walk(folder):
        for f in names:
            path = os.path.join(root, f)
            try:
                st = os.stat(path)
            except OSError:
                continue
            files.append((st.st_mtime, st.st_size, path))
    total = sum(f[1] for f in files)
    for _, size, path in sorted(files):
        if total <= limit:
            break
        try:
            os.remove(path)
            total -= size
        except OSError:
            pass


def prepare(c, r, flags):
    """A cell made ready for its build - the map, the land cover, the ground, the aerial and street pictures, the property
    rolls (mostly the services' time): its name, "" or what went wrong, and what the build takes."""
    name, projection, (s, w, n, e) = land(c, r)
    # (none started with less than --free GB, 120 by default, left on the game's drive: the rest of the list waits for the
    # next run, --skip-built)
    # (nor once the game is not where it was - moved to another drive: the packs would go into a folder of their own)
    if g_full.is_set() or not os.path.exists(os.path.join(O['game'], 'GTA5_Enhanced.exe')):
        g_full.set()
        return name, 'not started: the game is not at ' + O['game'], None
    pack = pack_of(name)
    for where in (os.path.splitdrive(pack)[0] + os.sep, O['worlds']):
        if shutil.disk_usage(where).free < float(O.get('free', 120)) * 1e9:
            g_full.set()
            return name, f'not started: {where} is nearly full', None
    folder = os.path.join(O['worlds'], name)
    os.makedirs(folder, exist_ok=True)
    log = os.path.join(folder, 'build.log')
    common = ['--game', O['game'], '--keys', O['keys']]
    world = os.path.join(folder, 'world.json')
    if not os.path.exists(world):
        if run([O['worldgen'], 'osm', *common, '--pbf', O['pbf'], '--box', f'{s:.9f},{w:.9f},{n:.9f},{e:.9f}', '--projection',
                ','.join(f'{v:.9f}' for v in projection), '--out', folder, '--name', name], log):
            return name, f'osm failed (see {log})', None
        if run([sys.executable, '-I', O['msbuildings'], 'merge', '--world', world, '--cells', O['mscells']], log):
            os.remove(world)  # (made again, both steps, next time)
            return name, f'footprints failed (see {log})', None
    cover = os.path.join(folder, 'landcover.bin')
    if not os.path.exists(cover) and run([O['venv'], '-I', O['nlcd'], '--world', world, '--out', cover], log):
        return name, f'land cover failed (see {log})', None
    ground = os.path.join(folder, 'ground.bin')
    # (the 1 m lidar DEM sampled every 2 m into 4 m cells: the yards' banks, the roads' crests; its tiles of the state's
    # lattice in the shared cache - 3.3 km round the box: the build works the water and the ground out 3 km past it, so that
    # neighbours' bands are the same -, six asked for at a time)
    if not os.path.exists(ground) and run([O['venv'], '-I', O['dem'], '--world', world, '--out', ground, '--step', '2', '--cell', '4',
                                           '--margin', '3300', '--cache', os.path.join(O['caches'], 'dem'), '--fetch', '6'], log):
        return name, f'ground failed (see {log})', None
    photo = os.path.join(folder, 'photo')
    if not os.path.exists(os.path.join(photo, 'cover.bin')) and run([O['venv'], '-I', O['photo'], '--world', world, '--out', photo,
                                                                     '--cache', os.path.join(O['caches'], 'naip'), '--landcover', cover,
                                                                     '--fetch', '6'], log):
        return name, f'aerial pictures failed (see {log})', None
    # (the walls the street pictures show, when any were taken in the sector)
    walls = []
    if O.get('mapillary'):
        seen = os.path.join(folder, 'facades.json')
        if run([O['venv'], '-I', O['photofacade'], '--world', world, '--metadata', O['mapillary'], '--out', seen], log):
            return name, f'street pictures failed (see {log})', None
        with open(seen, encoding='utf-8') as f:
            if '"building"' in f.read(4096):
                walls = ['--facades', seen]
    # (what the property rolls say of its buildings - vacant lots, mobile homes, storeys, the year built -, when the counties'
    # parcels are there)
    rolls = []
    if os.path.exists(O['parcelIndex']):
        parcels = os.path.join(folder, 'parcels.json')
        if run([O['venv'], '-I', O['parcels'], 'join', '--world', world, '--index', O['parcelIndex'], '--out', parcels], log):
            return name, f'property rolls failed (see {log})', None
        rolls = ['--parcels', parcels]
    return name, '', {'folder': folder, 'log': log, 'world': world, 'cover': cover, 'ground': ground, 'photo': photo, 'extra': walls + rolls}


def make(c, r, flags, ready):
    """A prepared cell built and packed (the CPU's time): its name, and "" when it was built, else what went wrong."""
    name = land(c, r)[0]
    folder, log, world, cover, ground, photo = (ready[k] for k in ('folder', 'log', 'world', 'cover', 'ground', 'photo'))
    common = ['--game', O['game'], '--keys', O['keys']]
    pack = pack_of(name)
    gen = os.path.join(folder, 'full_gen')
    shutil.rmtree(gen, ignore_errors=True)  # (an earlier, failed run's files are not packed with this one's)
    if run([O['worldgen'], 'build', *common, '--world', world, '--out', gen, '--name', name, '--open', 'nsew', '--core', ','.join(f'{v:.1f}' for v in CORE),
            '--landcover', cover,
            '--ground', ground, '--photo', photo, *ready['extra'], '--navmesh', 'near', '--climate', 'tropical'], log):
        return name, f'build failed (see {log})'
    if not os.path.exists(os.path.join(O['game'], 'GTA5_Enhanced.exe')):
        g_full.set()
        return name, 'not packed: the game is not at ' + O['game']
    os.makedirs(os.path.dirname(pack), exist_ok=True)
    out = pack + '.new' if os.path.exists(pack) else pack
    if run([O['worldgen'], 'pack', *common, '--in', gen, '--out', out, '--name', name], log):
        return name, f'pack failed (see {log})'
    # (its population zones into the land.json next to the pack)
    if run([sys.executable, '-I', O['zones'], '--world', world, '--land', os.path.join(folder, name + '_land.png'), '--landcover', cover, '--out',
            os.path.join(os.path.dirname(pack), 'land.json')], log):
        return name, f'zones failed (see {log})'
    if 'keep' not in flags:
        shutil.rmtree(gen, ignore_errors=True)
        shutil.rmtree(photo, ignore_errors=True)  # (about 200 MB a sector: made again from the pictures if wanted)
        shutil.rmtree(os.path.join(folder, 'dem_cache'), ignore_errors=True)  # (an earlier run's own cache)
        # (and the build's lists - the props, the roads' wear, the buildings found: 30 MB a sector, for looking into one)
        for f in ('world.json', name + '_props.txt', name + '_wear.txt', name + '_found.txt'):
            try:
                os.remove(os.path.join(folder, f))
            except OSError:
                pass
    return name, ''


def survey(out):
    """The cells with land: worldgen survey of the state's extract on the lattice (what each cell has: coast, roads,
    buildings, water), then the sea taken away - the cells reachable from the lattice's edge through cells with no road,
    building or coastline -, the rest (land, marshes, lakes) kept."""
    grid = out + '.grid.tsv'
    if subprocess.run([O['worldgen'], 'survey', '--game', O['game'], '--keys', O['keys'], '--pbf', O['pbf'], '--box', '24.3,-87.7,31.1,-79.8', '--grid',
                       f'{LAT0},{LON0},{DLAT:.9f},{DLON:.9f}', '--out', grid]).returncode:
        sys.exit('survey failed')
    counts = {}
    with open(grid, encoding='utf-8') as f:
        header = f.readline().rstrip('\n').split('\t')
        for line in f:
            v = dict(zip(header, line.rstrip('\n').split('\t')))
            counts[(int(v['column']), int(v['row']))] = (int(v['coast']), int(v['roads']), int(v['buildings']))
    c0, c1 = min(c for c, r in counts) - 1, max(c for c, r in counts) + 1
    r0, r1 = min(r for c, r in counts) - 1, max(r for c, r in counts) + 1
    empty = lambda c, r: not any(counts.get((c, r), (0, 0, 0)))
    sea = set()
    queue = collections.deque([(c, r) for c in range(c0, c1 + 1) for r in (r0, r1)] + [(c, r) for r in range(r0, r1 + 1) for c in (c0, c1)])
    while queue:
        c, r = queue.popleft()
        if (c, r) in sea or not (c0 <= c <= c1 and r0 <= r <= r1) or not empty(c, r):
            continue
        sea.add((c, r))
        queue.extend(((c + 1, r), (c - 1, r), (c, r + 1), (c, r - 1)))
    kept = [(c, r) for r in range(r0, r1 + 1) for c in range(c0, c1 + 1) if (c, r) not in sea]
    with open(out, 'w', encoding='utf-8') as f:
        f.write('column\trow\tkind\troads\tbuildings\n')
        for c, r in kept:
            k = counts.get((c, r), (0, 0, 0))
            f.write(f'{c}\t{r}\tland\t{k[1]}\t{k[2]}\n')
    print(f'{len(kept)} land cells of {len(counts)} with something in them ({len(sea)} of the sea taken away)')


def main():
    if len(sys.argv) == 3 and sys.argv[1] == '--survey':
        survey(sys.argv[2])
        return
    flags = options(sys.argv[1:])
    background()
    named = {}
    for item in filter(None, O['names'].split(';')):
        cell, name = item.split('=')
        c, r = (int(v) for v in cell.split(','))
        named[(c, r)] = name
    cells = []
    if 'list' in O:
        with open(O['list'], encoding='utf-8') as f:
            header = f.readline().rstrip('\n').split('\t')
            ci, ri = header.index('column'), header.index('row')
            for line in f:
                p = line.rstrip('\n').split('\t')
                if len(p) > max(ci, ri):
                    cells.append((int(p[ci]), int(p[ri])))
        home = tuple(int(v) for v in O['home'].split(',')) if 'home' in O else HOME
        cells.sort(key=lambda t: ((t[0] - home[0]) ** 2 + (t[1] - home[1]) ** 2, t))
        first = [tuple(int(v) for v in item.split(',')) for item in filter(None, O.get('first', '').split(';'))]
        cells.sort(key=lambda t: first.index(t) if t in first else len(first))
    elif 'cells' in O:
        c0, r0, c1, r1 = (int(v) for v in O['cells'].split(','))
        cells = [(c, r) for r in range(r0, r1 + 1) for c in range(c0, c1 + 1)]
    else:
        sys.exit(__doc__)
    again = []
    if 'rebuild' in O:
        with open(O['rebuild'], encoding='utf-8') as f:
            header = f.readline().rstrip('\n').split('\t')
            ci, ri = header.index('column'), header.index('row')
            for line in f:
                p = line.rstrip('\n').split('\t')
                if len(p) > max(ci, ri) and (int(p[ci]), int(p[ri])) not in again:
                    again.append((int(p[ci]), int(p[ri])))
    todo = list(again)
    for c, r in cells:
        name = named.get((c, r), land(c, r)[0])
        if (c, r) in named or (c, r) in again:
            continue
        if 'skip-built' in flags and os.path.exists(pack_of(name)):
            continue
        todo.append((c, r))
    say(f'{len(cells)} cells, {len(todo)} to build ({len(again)} of them again) with {O["jobs"]} build(s) and {O["prep"]} made ready at a time')
    if 'dry-run' in flags:
        for c, r in todo[:20]:
            print(f'  {c},{r}')
        return
    start, done, failed = time.time(), 0, 0
    jobs, prep = int(O['jobs']), int(O['prep'])
    # (a cell is made ready while others build; at most jobs + prep + 2 under way - their files on the disk - and the
    # builds in the order the cells were made ready)
    room = threading.Semaphore(jobs + prep + 2)
    finished = queue.Queue()

    def build_it(c, r, ready):
        try:
            name, error = make(c, r, flags, ready)
        except Exception as e:
            name, error = land(c, r)[0], f'build raised {e!r}'
        finally:
            room.release()
        finished.put((c, r, name, error))

    def ready_it(c, r):
        room.acquire()
        try:
            name, error, ready = prepare(c, r, flags)
        except Exception as e:
            name, error, ready = land(c, r)[0], f'making ready raised {e!r}', None
        if error:
            room.release()
            finished.put((c, r, name, error))
            return
        builds.submit(build_it, c, r, ready)

    caches = [os.path.join(O['caches'], k) for k in ('dem', 'naip')]
    with concurrent.futures.ThreadPoolExecutor(max_workers=jobs) as builds, concurrent.futures.ThreadPoolExecutor(max_workers=prep) as readying:
        for c, r in todo:
            readying.submit(ready_it, c, r)
        for _ in todo:
            c, r, name, error = finished.get()
            done += 1
            failed += bool(error)
            spent = time.time() - start
            left = spent / done * (len(todo) - done)
            say(f'{done}/{len(todo)} {c},{r} {name}: {error or "built"} ({spent / 3600:.1f} h so far, about {left / 3600:.1f} h left)')
            for folder in caches:
                trim(folder, float(O['cachegb']) * 1e9 / len(caches))
    say(f'finished: {done - failed} built, {failed} failed')


if __name__ == '__main__':
    main()
