"""Finds the shared identity helpers in a script (research/char4-design.md §2.1): is_playable (c < 3), get_cur
(update_cur then load +4321), update_cur and the functions it calls; counts the calls to each.
usage: char4_helpers.py <script.ysc>"""
import os, struct, sys
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from ysc_write import Program

IS_PLAYABLE = bytes([45, 1, 3, 0, 0, 56, 0, 116, 12, 46, 1, 1])  # ENTER 1 3; LOCAL_U8_LOAD 0; PUSH 3; ILT; LEAVE 1 1
CUR_LOAD = bytes([97, 0x2E, 0xC1, 0x01, 70]) + struct.pack('<h', 2370) + bytes([70]) + struct.pack('<h', 539) + bytes([71]) + struct.pack('<h', 4321)


def decode(code):
    """(address, opcode, length) for every instruction (same rules as tools/ysc_dis.py)."""
    simple = set(range(0, 37)) | {42, 43, 47, 48, 49, 50, 51, 63, 102, 103, 108, 109, 110, 111, 130} | set(range(112, 130))
    u8 = {37, 52, 53, 54, 55, 56, 57, 58, 59, 60, 61, 62, 64, 65, 66, 104, 105, 106, 107}
    s16 = set(range(67, 73)) | set(range(73, 85)) | set(range(85, 93))
    u24 = {93} | set(range(94, 101))
    ip, out = 0, []
    while ip < len(code):
        op = code[ip]
        if op in simple:
            n = 1
        elif op in u8:
            n = 2
        elif op == 38:
            n = 3
        elif op == 39:
            n = 4
        elif op in (40, 41):
            n = 5
        elif op == 44:
            n = 4
        elif op == 45:
            n = 5 + code[ip + 4]
        elif op == 46:
            n = 3
        elif op in s16:
            n = 3
        elif op in u24:
            n = 4
        elif op == 101:
            n = 2 + 6 * code[ip + 1]
        else:
            n = 1
        out.append((ip, op, n))
        ip += n
    return out


def analyse(p):
    code = bytes(p.code)
    ins = decode(code)
    enters = [a for a, op, n in ins if op == 45]
    def owner(addr):
        lo, hi = 0, len(enters) - 1
        while lo < hi:
            mid = (lo + hi + 1) // 2
            if enters[mid] <= addr:
                lo = mid
            else:
                hi = mid - 1
        return enters[lo]
    calls = {}  # target -> [call sites]
    for a, op, n in ins:
        if op == 93:
            t = code[a + 1] | code[a + 2] << 8 | code[a + 3] << 16
            calls.setdefault(t, []).append(a)
    is_playable = [a for a in enters if code[a:a + len(IS_PLAYABLE)] == IS_PLAYABLE]
    get_cur = [a for a in enters if code[a + 5] == 93 and code[a + 9:a + 9 + len(CUR_LOAD)] == CUR_LOAD and code[a + 9 + len(CUR_LOAD)] == 46]
    result = {'is_playable': is_playable, 'get_cur': get_cur, 'calls': calls, 'owner': owner, 'enters': enters, 'ins': ins}
    if get_cur:
        update_cur = code[get_cur[0] + 6] | code[get_cur[0] + 7] << 8 | code[get_cur[0] + 8] << 16
        # update_cur and every function reachable from it: the rewrite leaves their calls alone.
        private, todo = set(), [update_cur]
        while todo:
            f = todo.pop()
            if f in private:
                continue
            private.add(f)
            end = enters[enters.index(f) + 1] if enters.index(f) + 1 < len(enters) else len(code)
            for a, op, n in ins:
                if f <= a < end and op == 93:
                    todo.append(code[a + 1] | code[a + 2] << 8 | code[a + 3] << 16)
        result['update_cur'] = update_cur
        result['private'] = private
    return result


if __name__ == '__main__':
    p = Program(sys.argv[1])
    r = analyse(p)
    print(p.name, 'code', len(p.code))
    for k in ('is_playable', 'get_cur'):
        for f in r[k]:
            sites = r['calls'].get(f, [])
            inside = [s for s in sites if r['owner'](s) in r.get('private', set())]
            print('  %s @%d: %d calls (%d inside update_cur and its helpers)' % (k, f, len(sites), len(inside)))
    if 'update_cur' in r:
        print('  update_cur @%d, private helpers %s' % (r['update_cur'], sorted(r['private'])))
