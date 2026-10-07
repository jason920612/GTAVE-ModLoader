"""Learn how legacy game scripts map onto Enhanced ones (research/phase0.md §26), from the same scripts of both games.
usage: learn_ysc.py <legacy .ysc folder> <Enhanced .ysc folder> <out.json>

Both versions use the same bytecode; what differs is
  - native hashes: every native has another hash in Enhanced,
  - global variable indices: blocks of globals moved.
The instruction streams of each script pair are aligned (opcodes only) and the operands of matching NATIVE and
GLOBAL_* instructions are paired up. Output: {"natives": {legacy: enhanced}, "globals": [[legacy start, enhanced start,
length], ...], "stats": ...} (hashes as hex strings)."""
import collections, difflib, json, os, sys, struct
sys.path.insert(0, os.path.dirname(__file__))
from ysc import Prog

SIMPLE = set(range(0, 37)) | {42, 43, 47, 48, 49, 50, 51, 63, 102, 103, 108, 109, 110, 111, 130} | set(range(112, 130))
U8 = {37, 52, 53, 54, 55, 56, 57, 58, 59, 60, 61, 62, 64, 65, 66, 104, 105, 106, 107}
S16 = {67, 68, 69, 70, 71, 72}
U16 = set(range(73, 85))
JMP = set(range(85, 93))
U24 = set(range(94, 101))
GLOBAL16 = {82, 83, 84}
GLOBAL24 = {97, 98, 99}


def rot_back(v, n):
    """A native table entry is the hash rotated right by (code size + index) & 63; rotate it back."""
    n &= 63
    return ((v << n) | (v >> (64 - n))) & 0xFFFFFFFFFFFFFFFF if n else v


def instructions(p):
    """(opcode, operand) per instruction; operand: native hash, global index or None."""
    c = p.code
    ip = 0
    out = []
    while ip < len(c):
        op = c[ip]
        val = None
        if op in SIMPLE:
            size = 1
        elif op in U8:
            size = 2
        elif op == 38:
            size = 3
        elif op == 39:
            size = 4
        elif op in (40, 41):
            size = 5
        elif op == 44:
            idx = (c[ip + 2] << 8) | c[ip + 3]
            val = rot_back(p.natives[idx], p.codesize + idx) if idx < len(p.natives) else None
            size = 4
        elif op == 45:
            size = 5 + c[ip + 4]
        elif op == 46:
            size = 3
        elif op in S16 or op in U16 or op in JMP:
            if op in GLOBAL16:
                val = struct.unpack_from("<H", c, ip + 1)[0]
            size = 3
        elif op == 93:
            size = 4
        elif op in U24:
            if op in GLOBAL24:
                val = c[ip + 1] | c[ip + 2] << 8 | c[ip + 3] << 16
            size = 4
        elif op == 101:
            size = 2 + 6 * c[ip + 1]
        else:
            size = 1
        # Opcode classes, so GLOBAL_U16 and GLOBAL_U24 of the same global still align.
        kind = 'G' + str((op - 82) % 3 if op in GLOBAL16 else (op - 97) % 3) if op in GLOBAL16 | GLOBAL24 else op
        out.append((kind, val))
        ip += size
    return out


def functions(ins):
    """Instruction lists per function (each starts at ENTER)."""
    out = []
    for i in ins:
        if i[0] == 45 or not out:
            out.append([])
        out[-1].append(i)
    return out


def align_lists(a, b):
    ka = [k for k, _ in a]
    kb = [k for k, _ in b]
    if ka == kb:
        return list(zip(a, b))
    if len(ka) * len(kb) > 2e7:
        return []
    pairs = []
    for i, j, size in difflib.SequenceMatcher(None, ka, kb, autojunk=False).get_matching_blocks():
        pairs += zip(a[i:i + size], b[j:j + size])
    return pairs


def align(a, b):
    """Matching instructions of two versions of a script: functions are matched by their opcode sequence, then
    aligned one by one."""
    fa, fb = functions(a), functions(b)
    sig = lambda f: hash(tuple(k for k, _ in f))
    pairs = []
    for i, j, size in difflib.SequenceMatcher(None, [sig(f) for f in fa], [sig(f) for f in fb], autojunk=False).get_matching_blocks():
        for k in range(size):
            pairs += zip(fa[i + k], fb[j + k])
    # Functions that changed: align those between matched neighbours by position.
    ma = mb = 0
    for i, j, size in difflib.SequenceMatcher(None, [sig(f) for f in fa], [sig(f) for f in fb], autojunk=False).get_matching_blocks():
        if i - ma == j - mb:
            for k in range(i - ma):
                pairs += align_lists(fa[ma + k], fb[mb + k])
        ma, mb = i + size, j + size
    return pairs


def pair(args):
    """Native and global operand pairs of one script (legacy, Enhanced)."""
    legacy_dir, enhanced_dir, name = args
    natives = collections.Counter()
    globals_ = collections.Counter()
    try:
        a = instructions(Prog(os.path.join(legacy_dir, name)))
        b = instructions(Prog(os.path.join(enhanced_dir, name)))
    except Exception as e:
        print("skip", name, e, file=sys.stderr)
        return natives, globals_
    for (ko, va), (_, vb) in align(a, b):
        if va is None or vb is None:
            continue
        if ko == 44:
            natives[(va, vb)] += 1
        else:
            globals_[(va, vb)] += 1
    return natives, globals_


def main():
    import multiprocessing
    legacy_dir, enhanced_dir, out_path = sys.argv[1:4]
    names = sorted(set(os.listdir(legacy_dir)) & set(os.listdir(enhanced_dir)))
    natives = collections.defaultdict(collections.Counter)
    globals_ = collections.defaultdict(collections.Counter)
    with multiprocessing.Pool() as pool:
        for n, (na, gl) in enumerate(pool.imap_unordered(pair, [(legacy_dir, enhanced_dir, x) for x in names])):
            for (va, vb), c in na.items():
                natives[va][vb] += c
            for (va, vb), c in gl.items():
                globals_[va][vb] += c
            if n % 50 == 0:
                print(f"{n}/{len(names)}: natives {len(natives)} globals {len(globals_)}", file=sys.stderr)

    native_map = {}
    ambiguous = 0
    for legacy, counts in natives.items():
        (best, c), *rest = counts.most_common()
        if rest and rest[0][1] * 4 > c:
            ambiguous += 1
        native_map[f"{legacy:016x}"] = f"{best:016x}"
    # Globals: most common target per index, then runs of consecutive indices with the same shift.
    shifts = {g: counts.most_common(1)[0][0] - g for g, counts in globals_.items()}
    runs = []
    for g in sorted(shifts):
        d = shifts[g]
        if runs and runs[-1][1] - runs[-1][0] == d and g - runs[-1][0] <= runs[-1][2] + 4096:
            runs[-1][2] = g - runs[-1][0] + 1
        else:
            runs.append([g, g + d, 1])
    exact = {}
    global_ambiguous = 0
    for g, counts in globals_.items():
        (best, c), *rest = counts.most_common()
        if rest and rest[0][1] * 4 > c:
            global_ambiguous += 1
        exact[str(g)] = best
    json.dump({"natives": native_map, "globals": runs, "global_map": exact, "global_ambiguous": global_ambiguous,
               "stats": {"scripts": len(names), "natives": len(native_map), "ambiguous_natives": ambiguous,
                         "globals_seen": len(shifts), "global_runs": len(runs)}}, open(out_path, "w"), indent=0)
    print("natives", len(native_map), "ambiguous", ambiguous, "globals", len(shifts), "runs", len(runs))


if __name__ == "__main__":
    main()
