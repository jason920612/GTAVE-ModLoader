"""World builder sectors (research/phase0.md §62): Florida cut into boxes the size of the new land's place in the game's
world (x 4300..16033, y -3000..10405), each built as a pack of its own; the loader (game/sectors) keeps one of them there
and switches to the next one where the player crosses their shared edge.

The grid is anchored at Miami's box (its south-west corner 25.745 N, -80.232 E): a sector is 0.121 degrees of latitude
by 0.117 of longitude, cell (column, row) = (0, 0) Miami, (0, 1) the one north of it, (-1, 0) west. For each cell:
its OpenStreetMap extract (worldgen osm), its build (the light kind: the map's buildings as the game's models, no laser
scan or aerial pictures; every edge open: the land goes on into the neighbours) and its pack (ModLoader\\mods\\<name>,
dlc.rpf.new when a pack is there already: the loader takes it at the next start).

usage: python -I tools/worldgen/sectors.py --cells c0,r0,c1,r1 [--skip-built] [--dry-run]
  --names col,row=name;...  existing packs for some cells (not built again; by default Miami, the one north of it and the
                            two test sectors west and south of it)
Paths: --game, --worldgen, --keys, --pbf, --catalog, --worlds (the world folders) as below by default.
"""
import os, subprocess, sys, time

O = {
    'game': r'D:\SteamLibrary\steamapps\common\Grand Theft Auto V Enhanced',
    'worldgen': os.path.join(os.path.dirname(os.path.abspath(__file__)), '..', '..', 'build', 'Release', 'worldgen.exe'),
    'keys': r'D:\GTAV-UE5\keys\ng.bin',
    'pbf': r'D:\GTAV-UE5\osm\pbf\florida.osm.pbf',
    'catalog': r'D:\GTAV-UE5\catalog.tsv',
    'worlds': r'D:\GTAV-UE5\world',
    'names': '0,0=wbmia;0,1=wbnmb;-1,0=wbfl_099_100;0,-1=wbfl_100_099',
}
LAT0, LON0, DLAT, DLON = 25.745, -80.232, 0.121, 0.117


def options(argv):
    flags = set()
    i = 0
    while i < len(argv):
        a = argv[i]
        if a in ('--skip-built', '--dry-run'):
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


def main():
    flags = options(sys.argv[1:])
    if 'cells' not in O:
        sys.exit(__doc__)
    c0, r0, c1, r1 = (int(v) for v in O['cells'].split(','))
    named = {}
    for item in filter(None, O['names'].split(';')):
        cell, name = item.split('=')
        c, r = (int(v) for v in cell.split(','))
        named[(c, r)] = name
    game_mods = os.path.join(O['game'], 'ModLoader', 'mods')
    for r in range(r0, r1 + 1):
        for c in range(c0, c1 + 1):
            if (c, r) in named:
                print(f'cell {c},{r}: {named[(c, r)]} (built already)', flush=True)
                continue
            name = f'wbfl_{100 + c:03d}_{100 + r:03d}'
            s, w = LAT0 + r * DLAT, LON0 + c * DLON
            n, e = s + DLAT, w + DLON
            folder = os.path.join(O['worlds'], name)
            pack = os.path.join(game_mods, name, 'dlc.rpf')
            if 'skip-built' in flags and os.path.exists(pack):
                print(f'cell {c},{r}: {name} built already', flush=True)
                continue
            print(f'cell {c},{r}: {name} ({s:.3f}, {w:.3f})..({n:.3f}, {e:.3f})', flush=True)
            if 'dry-run' in flags:
                continue
            os.makedirs(folder, exist_ok=True)
            log = os.path.join(folder, 'build.log')
            start = time.time()
            common = ['--game', O['game'], '--keys', O['keys']]
            if not os.path.exists(os.path.join(folder, 'world.json')):
                if run([O['worldgen'], 'osm', *common, '--pbf', O['pbf'], '--box', f'{s},{w},{n},{e}', '--out', folder, '--name', name], log):
                    print(f'  osm failed (see {log})', flush=True)
                    continue
            # (open sea: no roads and no buildings in the extract - nothing to build)
            summary = [line for line in open(log, encoding='utf-8', errors='replace') if ' roads, ' in line and ' buildings, ' in line]
            if summary:
                words = summary[-1].replace(',', '').split()
                roads, buildings = int(words[words.index('roads') - 1]), int(words[words.index('buildings') - 1])
                if roads == 0 and buildings == 0:
                    print('  open sea: left out', flush=True)
                    continue
            gen = os.path.join(folder, 'full_gen')
            if run([O['worldgen'], 'build', *common, '--world', os.path.join(folder, 'world.json'), '--out', gen, '--name', name,
                    '--catalog', O['catalog'], '--open', 'nsew'], log):
                print(f'  build failed (see {log})', flush=True)
                continue
            os.makedirs(os.path.dirname(pack), exist_ok=True)
            out = pack + '.new' if os.path.exists(pack) else pack
            if run([O['worldgen'], 'pack', *common, '--in', gen, '--out', out, '--name', name], log):
                print(f'  pack failed (see {log})', flush=True)
                continue
            print(f'  done in {time.time() - start:.0f} s', flush=True)


if __name__ == '__main__':
    main()
