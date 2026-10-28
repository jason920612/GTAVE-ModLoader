"""Two neighbouring lands' shared band compared (research/phase0.md §65): every map entity each build made (worldgen
entities, in the real world's place - the state's projection's plane, sectors.py), in the part of the world both lands'
boxes take in (less a margin at each box's edge, where each cuts what crosses it), matched one to one - the game's models
by name and place, the build's own (named after the land) by place, turn and box. Crossing without a cut shows the one
land's band, then the other's in the same place: what does not match is what the player would see change.

usage: python -I tools/worldgen/banddiff.py <column,row> <column,row> [--margin metres] [--show n] [--tolerance metres]
  each land's build kept (sectors.py --keep: <worlds>\\fl_<column>_<row>\\full_gen); its entities written next to it
  (entities.tsv) once.
"""
import collections
import math
import os
import subprocess
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import sectors  # noqa: E402


def entities(c, r):
    """A land's entities in the projection's plane: dicts of map, archetype, x, y, z, q (x, y, z, w), scales, box."""
    name, (_, _, x0, y0, _, _), _ = sectors.land(c, r)
    folder = os.path.join(sectors.O['worlds'], name)
    gen = os.path.join(folder, 'full_gen')
    tsv = os.path.join(folder, 'entities.tsv')
    if not os.path.exists(tsv) or os.path.getmtime(tsv) < os.path.getmtime(gen):
        if not os.path.isdir(gen):
            sys.exit(f'no build of {name} kept ({gen}): sectors.py --cells {c},{r},{c},{r} --keep')
        if subprocess.run([sectors.O['worldgen'], 'entities', '--game', sectors.O['game'], '--keys', sectors.O['keys'], '--in', gen, '--out', tsv,
                           '--origin', f'{x0:.3f},{y0:.3f}']).returncode:
            sys.exit(f'worldgen entities failed for {name}')
    out = []
    with open(tsv, encoding='utf-8') as f:
        header = f.readline().rstrip('\n').split('\t')
        for line in f:
            v = dict(zip(header, line.rstrip('\n').split('\t')))
            out.append({'map': v['map'][len(name) + 1:] if v['map'].startswith(name + '_') else v['map'], 'archetype': v['archetype'],
                        'p': (float(v['x']), float(v['y']), float(v['z'])), 'q': tuple(float(v[k]) for k in ('qx', 'qy', 'qz', 'qw')),
                        's': (float(v['scaleXY']), float(v['scaleZ'])), 'box': v['box']})
    return name, (x0, y0), out


def kind(map_name):
    """What a map holds, from its name: lod, hd, n (near cells), far (1 km blocks), or its first word."""
    first = map_name.split('_')[0]
    if first in ('lod', 'hd', 'n'):
        return first
    return 'far' if first.isdigit() else first


def same_turn(a, b, tol=0.002):
    d = sum(x * y for x, y in zip(a, b))
    return abs(abs(d) - 1) < tol


def same_box(a, b, tol=0.03):
    if a == '-' or b == '-':
        return a == b
    return all(abs(float(x) - float(y)) <= tol for x, y in zip(a.split(','), b.split(',')))


def main():
    args = sys.argv[1:]
    opts = {'margin': 300.0, 'show': 12, 'tolerance': 0.05}
    cells = []
    while args:
        a = args.pop(0)
        if a.startswith('--'):
            opts[a[2:]] = float(args.pop(0))
        else:
            cells.append(tuple(int(v) for v in a.split(',')))
    if len(cells) != 2:
        sys.exit(__doc__)
    lands = [entities(*c) for c in cells]
    # (the boxes in the plane, their common part less the margin)
    boxes = [(sectors.BOX[0] - o[0], sectors.BOX[1] - o[1], sectors.BOX[2] - o[0], sectors.BOX[3] - o[1]) for _, o, _ in lands]
    m = opts['margin']
    region = (max(boxes[0][0], boxes[1][0]) + m, max(boxes[0][1], boxes[1][1]) + m, min(boxes[0][2], boxes[1][2]) - m, min(boxes[0][3], boxes[1][3]) - m)
    if region[0] >= region[2] or region[1] >= region[3]:
        sys.exit(f'the two lands share no band (their boxes: {boxes})')
    print(f'{lands[0][0]} and {lands[1][0]}: the band {region[0]:.0f}..{region[2]:.0f} x {region[1]:.0f}..{region[3]:.0f} '
          f'({(region[2] - region[0]) / 1000:.2f} x {(region[3] - region[1]) / 1000:.2f} km)')
    inside = lambda e: region[0] <= e['p'][0] < region[2] and region[1] <= e['p'][1] < region[3]
    a = [e for e in lands[0][2] if inside(e)]
    b = [e for e in lands[1][2] if inside(e)]
    # (the game's models: an archetype both lands use; the build's own: one only its land has)
    ours_a = {e['archetype'] for e in lands[0][2] if e['box'] != '-'}
    ours_b = {e['archetype'] for e in lands[1][2] if e['box'] != '-'}
    tol = opts['tolerance']
    cell = lambda p: (math.floor(p[0]), math.floor(p[1]))
    grid = collections.defaultdict(list)
    for i, e in enumerate(b):
        grid[cell(e['p'])].append(i)
    used = [False] * len(b)
    matched = collections.Counter()
    total_a, total_b = collections.Counter(kind(e['map']) for e in a), collections.Counter(kind(e['map']) for e in b)
    missing_a = []
    for e in a:
        x, y = cell(e['p'])
        hit = None
        for dx in (-1, 0, 1):
            for dy in (-1, 0, 1):
                for i in grid.get((x + dx, y + dy), ()):
                    o = b[i]
                    if used[i] or max(abs(u - v) for u, v in zip(e['p'], o['p'])) > tol:
                        continue
                    if e['archetype'] in ours_a or o['archetype'] in ours_b:
                        if not (e['archetype'] in ours_a and o['archetype'] in ours_b and same_box(e['box'], o['box'])):
                            continue
                    elif e['archetype'] != o['archetype']:
                        continue
                    if not same_turn(e['q'], o['q']) or abs(e['s'][0] - o['s'][0]) > 0.01 or abs(e['s'][1] - o['s'][1]) > 0.01:
                        continue
                    hit = i
                    break
                if hit is not None:
                    break
            if hit is not None:
                break
        if hit is None:
            missing_a.append(e)
        else:
            used[hit] = True
            matched[kind(e['map'])] += 1
    missing_b = [o for i, o in enumerate(b) if not used[i]]
    # why the rest did not match: what the other land has at the same place (within the tolerance) - or nothing there
    unmatched_b = collections.defaultdict(list)
    for i, o in enumerate(b):
        if not used[i]:
            unmatched_b[cell(o['p'])].append(o)
    reasons = collections.Counter()
    examples = collections.defaultdict(list)
    for e in missing_a:
        x, y = cell(e['p'])
        there = [o for dx in (-1, 0, 1) for dy in (-1, 0, 1) for o in unmatched_b.get((x + dx, y + dy), ())
                 if max(abs(u - v) for u, v in zip(e['p'][:2], o['p'][:2])) <= tol]
        if not there:
            why = 'nothing at its place'
        else:
            o = there[0]
            if abs(e['p'][2] - o['p'][2]) > tol:
                why = 'height'
            elif not same_turn(e['q'], o['q']):
                why = 'turn'
            elif abs(e['s'][0] - o['s'][0]) > 0.01 or abs(e['s'][1] - o['s'][1]) > 0.01:
                why = 'scale'
            elif (e['box'] == '-') != (o['box'] == '-') or (e['box'] == '-' and e['archetype'] != o['archetype']):
                why = 'model'
            else:
                d = [abs(float(u) - float(v)) for u, v in zip(e['box'].split(','), o['box'].split(','))]
                why = 'box ' + ('top' if max(range(6), key=lambda k: d[k]) == 5 else 'side')
        reasons[(kind(e['map']), why)] += 1
        if len(examples[(kind(e['map']), why)]) < 3:
            examples[(kind(e['map']), why)].append((e, there[0] if there else None))
    print('unmatched by why (the first land\'s):')
    for (k, why), n in sorted(reasons.items(), key=lambda t: -t[1]):
        print(f'  {k:>4} {why}: {n}')
        for e, o in examples[(k, why)]:
            print(f'       {e["map"]} {e["archetype"]} ({e["p"][0]:.2f}, {e["p"][1]:.2f}, {e["p"][2]:.2f}) box {e["box"]}' +
                  (f' | {o["map"]} {o["archetype"]} z {o["p"][2]:.2f} box {o["box"]}' if o else ''))
    print(f'{"kind":>6} {"in " + lands[0][0]:>16} {"in " + lands[1][0]:>16} {"matched":>9}')
    for k in sorted(set(total_a) | set(total_b)):
        print(f'{k:>6} {total_a[k]:>16} {total_b[k]:>16} {matched[k]:>9}')
    print(f'{"all":>6} {len(a):>16} {len(b):>16} {sum(matched.values()):>9}')
    for name, missing, other in ((lands[0][0], missing_a, b), (lands[1][0], missing_b, a)):
        if not missing:
            continue
        print(f'only in {name}: {len(missing)} ({dict(collections.Counter(kind(e["map"]) for e in missing))})')
        for e in missing[:int(opts['show'])]:
            # (the nearest of the other land's, for what differs)
            near_one = min(other, key=lambda o: (o['p'][0] - e['p'][0]) ** 2 + (o['p'][1] - e['p'][1]) ** 2, default=None)
            d = math.dist(e['p'], near_one['p']) if near_one else -1
            print(f'  {e["map"]} {e["archetype"]} ({e["p"][0]:.2f}, {e["p"][1]:.2f}, {e["p"][2]:.2f}) box {e["box"]}; nearest of the other: '
                  f'{near_one["map"] if near_one else "-"} {near_one["archetype"] if near_one else ""} {d:.2f} m away, box {near_one["box"] if near_one else ""}')


if __name__ == '__main__':
    main()
