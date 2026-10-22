"""M0 U7 prototype of the hash-triple rule (research/char4-design.md §3.4): finds every SWITCH on a character whose cases
0, 1 and 2 differ only in a pushed stat hash SP0_X / SP1_X / SP2_X, and gives it a case 3 with SP3_X. The SWITCH table
cannot grow in place, so each function holding such a SWITCH is copied to the end of the code (F', relative jumps
recomputed, the new case bodies appended) and the original body becomes a forwarder to it.
usage: char4_triples.py <in.ysc> <out.ysc> <spstatssetup.xml> [<sp3 stats xml to write>]"""
import os, re, struct, sys
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from ysc_write import Program
from char4_helpers import decode
from convert_ytd import joaat

PAGE = 0x4000
JUMPS = set(range(85, 93))  # J, JZ, IEQ_JZ ... ILE_JZ: s16 relative to the next instruction


def signed(h):
    return h - (1 << 32) if h >= 1 << 31 else h


def stat_names(path):
    """SP0 hash -> (stat name, type), for every character stat."""
    out = {}
    for m in re.finditer(r'<stat\s+Name="([^"]+)"\s+Type="([^"]+)"[^>]*characterStat="true"', open(path, encoding='utf-8', errors='replace').read()):
        out[signed(joaat('SP0_' + m.group(1)))] = (m.group(1), m.group(2))
    return out


def instructions(code, start, end):
    return [(a, op, n) for a, op, n in decode(code[start:end])]


def switch_cases(code, at):
    n = code[at + 1]
    cases = []
    for k in range(n):
        value, off = struct.unpack_from('<ih', code, at + 2 + 6 * k)
        cases.append((value, at + 2 + 6 * (k + 1) + off))
    return cases


def body(code, ins, target):
    """The instructions of a case body: from its target to its first J (included)."""
    out = []
    started = False
    for a, op, n in ins:
        if a == target:
            started = True
        if started:
            out.append((a, op, n))
            if op == 85 or op == 46:
                return out
    return None


def triple(code, ins, at, names):
    """(stat name, case-2 body) when the SWITCH at `at` is a stat hash triple, else None."""
    cases = switch_cases(code, at)
    if sorted(v for v, _ in cases) != [0, 1, 2]:
        return None
    bodies = {v: body(code, ins, t) for v, t in cases}
    if any(b is None for b in bodies.values()):
        return None
    def shape(a, op, n):  # jumps by target (their offsets differ), the stat hash left out
        if op in JUMPS:
            return op, a + 3 + struct.unpack_from('<h', code, a + 1)[0]
        return op, b'' if op == 40 else code[a:a + n]
    shapes = {v: [shape(a, op, n) for a, op, n in b] for v, b in bodies.items()}
    if not (shapes[0] == shapes[1] == shapes[2]):
        return None
    pushes = {v: [struct.unpack_from('<i', code, a + 1)[0] for a, op, n in b if op == 40] for v, b in bodies.items()}
    if len(pushes[0]) != 1:
        return None
    name = names.get(pushes[0][0], (None, None))[0]
    if not name or pushes[1][0] != signed(joaat('SP1_' + name)) or pushes[2][0] != signed(joaat('SP2_' + name)):
        return None
    return name, bodies[2]


def relocate(code, start, end, new_at, extra):
    """The function [start, end) re-emitted at new_at; extra = {switch address: (name, case-2 body)} gets a case 3."""
    ins = instructions(code, start, end)
    # New address of every old instruction: switches grow by one case entry.
    where, pos = {}, new_at
    for a, op, n in ins:
        where[start + a] = pos
        pos += n + (6 if op == 101 and start + a in extra else 0)
    tail = pos  # case-3 bodies go after the function's last instruction
    bodies = {}
    for s, (name, b) in extra.items():
        bodies[s] = tail
        tail += sum(n for _, _, n in b)
    where[end] = pos  # (a jump to the function's end)
    out = bytearray()
    for a, op, n in ins:
        old = start + a
        here = where[old]
        if op in JUMPS:
            target = old + 3 + struct.unpack_from('<h', code, old + 1)[0]
            out += bytes([op]) + struct.pack('<h', where[target] - (here + 3))
        elif op == 101:
            cases = switch_cases(code, old)
            if old in extra:
                cases = cases + [(3, None)]
            out += bytes([101, len(cases)])
            for k, (value, target) in enumerate(cases):
                t = bodies[old] if target is None else where[target]
                out += struct.pack('<ih', value, t - (here + 2 + 6 * (k + 1)))
        else:
            out += code[old:old + n]
    for s, (name, b) in extra.items():
        for a, op, n in b:
            here = new_at + len(out)
            if op == 40:
                out += bytes([40]) + struct.pack('<i', signed(joaat('SP3_' + name)))
            elif op in JUMPS:
                target = a + 3 + struct.unpack_from('<h', code, a + 1)[0]
                out += bytes([op]) + struct.pack('<h', where[target] - (here + 3))
            else:
                out += code[a:a + n]
    return bytes(out)


def forwarder(code, start, end, target):
    """ENTER (as before), the parameters loaded, CALL target, LEAVE: the old body sends every call on to F'."""
    params = code[start + 1]
    enter = code[start:start + 5 + code[start + 4]]
    leaves = [(op, a) for a, op, n in instructions(code, start, end) if op == 46]
    rets = code[start + leaves[0][1] + 2]
    f = bytearray(enter)
    for i in range(params):
        f += bytes([56, i])  # LOCAL_U8_LOAD i
    f += bytes([93, target & 0xFF, (target >> 8) & 0xFF, target >> 16, 46, params, rets])
    return bytes(f) if len(f) <= end - start else None


def main():
    p = Program(sys.argv[1])
    names = stat_names(sys.argv[3])
    code = p.code
    all_ins = decode(bytes(code))
    starts = [i for i, (a, op, n) in enumerate(all_ins) if op == 45] + [len(all_ins)]
    found = {}  # (function start, end) -> {switch: (name, body)}
    for i in range(len(starts) - 1):
        ins = all_ins[starts[i]:starts[i + 1]]
        f = ins[0][0]
        end = all_ins[starts[i + 1]][0] if starts[i + 1] < len(all_ins) else len(code)
        for a, op, n in ins:
            if op == 101:
                t = triple(code, ins, a, names)
                if t:
                    found.setdefault((f, end), {})[a] = t
    used, moved, skipped = set(), 0, []
    for (f, end), extra in sorted(found.items()):
        size = len(relocate(code, f, end, len(code), extra))
        at = len(code)
        if at % PAGE + size > PAGE:
            code += bytes(PAGE - at % PAGE)
            at = len(code)
        new = relocate(code, f, end, at, extra)
        fwd = forwarder(code, f, end, at)
        if not fwd or size >= PAGE:
            skipped.append(f)
            continue
        code += new
        code[f:f + len(fwd)] = fwd
        code[f + len(fwd):end] = bytes(end - f - len(fwd))
        moved += 1
        used.update(name for name, _ in extra.values())
    open(sys.argv[2], 'wb').write(p.build())
    print('%s: %d triples in %d functions, %d moved, %d skipped (too short for a forwarder or too long), code -> %d' % (
        p.name, sum(len(e) for e in found.values()), len(found), moved, len(skipped), len(code)))
    if len(sys.argv) > 4:
        with open(sys.argv[4], 'w', encoding='utf-8', newline='\n') as out:
            out.write('<StatsSetup version="1">\n    <stats>\n')
            types = {n: ty for n, ty in names.values()}
            for name in sorted(used | {'TOTAL_CASH'}):
                out.write('\t\t<stat Name="SP3_%s" Type="%s" SaveCategory="0" online="false" profile="false" Owner="script" characterStat="false" />\n' % (name, types.get(name, 'int')))
            out.write('\t</stats>\n</StatsSetup>\n')
        print('%d SP3 stats written' % len(used | {'TOTAL_CASH'}))


if __name__ == '__main__':
    main()
