"""World builder sectors (research/phase0.md §62): Florida cut into boxes the size of the new land's place in the game's
world (x 4300..16033, y -3000..10405), each built as a pack of its own; the loader (game/sectors) keeps one of them there
and switches to the next one where the player crosses their shared edge.

The grid is anchored at Miami's box (its south-west corner 25.745 N, -80.232 E): a sector is 0.121 degrees of latitude
by 0.117 of longitude, cell (column, row) = (0, 0) Miami, (0, 1) the one north of it, (-1, 0) west. For each cell:
its OpenStreetMap extract (worldgen osm), its land cover (nlcd.py: NLCD's woods, wetlands, fields), its build (the
light kind: the map's buildings as the game's models, trees by the land cover, navmeshes only where people go, 16 m
terrain cells, no laser scan or aerial pictures; every edge open: the land goes on into the neighbours) and its pack
(ModLoader\\mods\\<name>; dlc.rpf.new when a pack is there already: the loader takes it at the next start). The build's
own files are removed once packed (--keep: kept).

usage: python -I tools/worldgen/sectors.py --cells c0,r0,c1,r1 | --list <cells.tsv> [--jobs n] [--skip-built] [--dry-run]
       [--keep]
  --list: the survey's land cells (worldgen survey, then the sea flood-filled away: a tsv with column and row), built
          nearest Miami first
  --names col,row=name;...  existing packs for some cells (not built again; by default Miami's full build)
Paths: --game, --worldgen, --keys, --pbf, --catalog, --worlds (the world folders), --log as below by default.
"""
import concurrent.futures, os, shutil, subprocess, sys, threading, time

O = {
    'game': r'D:\SteamLibrary\steamapps\common\Grand Theft Auto V Enhanced',
    'worldgen': os.path.join(os.path.dirname(os.path.abspath(__file__)), '..', '..', 'build', 'Release', 'worldgen.exe'),
    'nlcd': os.path.join(os.path.dirname(os.path.abspath(__file__)), 'nlcd.py'),
    'keys': r'D:\GTAV-UE5\keys\ng.bin',
    'pbf': r'D:\GTAV-UE5\osm\pbf\florida.osm.pbf',
    'catalog': r'D:\GTAV-UE5\catalog.tsv',
    'worlds': r'D:\GTAV-UE5\world',
    'names': '0,0=wbmia',
    'jobs': '1',
    'log': r'D:\GTAV-UE5\world\florida\sectors.log',
}
LAT0, LON0, DLAT, DLON = 25.745, -80.232, 0.121, 0.117
FLAGS = ('skip-built', 'dry-run', 'keep')
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
        return subprocess.run(cmd, stdout=out, stderr=subprocess.STDOUT).returncode


def build(c, r, flags):
    """One cell: its name, and "" when it was built, else what went wrong."""
    name = f'wbfl_{100 + c:03d}_{100 + r:03d}'
    s, w = LAT0 + r * DLAT, LON0 + c * DLON
    n, e = s + DLAT, w + DLON
    folder = os.path.join(O['worlds'], name)
    pack = os.path.join(O['game'], 'ModLoader', 'mods', name, 'dlc.rpf')
    os.makedirs(folder, exist_ok=True)
    log = os.path.join(folder, 'build.log')
    common = ['--game', O['game'], '--keys', O['keys']]
    world = os.path.join(folder, 'world.json')
    if not os.path.exists(world) and run([O['worldgen'], 'osm', *common, '--pbf', O['pbf'], '--box', f'{s},{w},{n},{e}', '--out', folder, '--name', name], log):
        return name, f'osm failed (see {log})'
    cover = os.path.join(folder, 'landcover.bin')
    if not os.path.exists(cover) and run([sys.executable, '-I', O['nlcd'], '--world', world, '--out', cover], log):
        return name, f'land cover failed (see {log})'
    gen = os.path.join(folder, 'full_gen')
    if run([O['worldgen'], 'build', *common, '--world', world, '--out', gen, '--name', name, '--catalog', O['catalog'], '--open', 'nsew',
            '--landcover', cover, '--navmesh', 'near', '--cell', '16'], log):
        return name, f'build failed (see {log})'
    os.makedirs(os.path.dirname(pack), exist_ok=True)
    out = pack + '.new' if os.path.exists(pack) else pack
    if run([O['worldgen'], 'pack', *common, '--in', gen, '--out', out, '--name', name], log):
        return name, f'pack failed (see {log})'
    if 'keep' not in flags:
        shutil.rmtree(gen, ignore_errors=True)
        for f in ('world.json',):
            try:
                os.remove(os.path.join(folder, f))
            except OSError:
                pass
    return name, ''


def main():
    flags = options(sys.argv[1:])
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
        cells.sort(key=lambda t: (t[0] ** 2 + t[1] ** 2, t))
    elif 'cells' in O:
        c0, r0, c1, r1 = (int(v) for v in O['cells'].split(','))
        cells = [(c, r) for r in range(r0, r1 + 1) for c in range(c0, c1 + 1)]
    else:
        sys.exit(__doc__)
    todo = []
    for c, r in cells:
        name = named.get((c, r), f'wbfl_{100 + c:03d}_{100 + r:03d}')
        if (c, r) in named:
            continue
        if 'skip-built' in flags and os.path.exists(os.path.join(O['game'], 'ModLoader', 'mods', name, 'dlc.rpf')):
            continue
        todo.append((c, r))
    say(f'{len(cells)} cells, {len(todo)} to build with {O["jobs"]} job(s)')
    if 'dry-run' in flags:
        for c, r in todo[:20]:
            print(f'  {c},{r}')
        return
    start, done, failed = time.time(), 0, 0
    with concurrent.futures.ThreadPoolExecutor(max_workers=int(O['jobs'])) as pool:
        futures = {pool.submit(build, c, r, flags): (c, r) for c, r in todo}
        for future in concurrent.futures.as_completed(futures):
            c, r = futures[future]
            name, error = future.result()
            done += 1
            failed += bool(error)
            spent = time.time() - start
            left = spent / done * (len(todo) - done)
            say(f'{done}/{len(todo)} {c},{r} {name}: {error or "built"} ({spent / 3600:.1f} h so far, about {left / 3600:.1f} h left)')
    say(f'finished: {done - failed} built, {failed} failed')


if __name__ == '__main__':
    main()
