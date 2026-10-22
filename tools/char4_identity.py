"""M0 U7 tracer: the identity rewrite (research/char4-design.md §3.2) of one script. Appends get_cur4 and is_char4,
which see the officer as character 3 while a native says the officer is the player, and points every call of get_cur
and is_playable at them, except the calls inside update_cur and the functions it uses (so G122220-2 never get 3).
The officer native is an appended table entry (GET_BENCHMARK_PASS, which the police mod overrides for this script).
usage: char4_identity.py <in.ysc> <out.ysc>"""
import os, struct, sys
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from ysc_write import Program
from char4_helpers import analyse

OFFICER = 0x7007C113A8466ADE  # game hash of GET_BENCHMARK_PASS (public 0x1B2366C3F2A5C8DF)
PAGE = 0x4000


def u24(v):
    return bytes([v & 0xFF, (v >> 8) & 0xFF, v >> 16])


def append(code, body_fn):
    """Appends a function built by body_fn(start) on one code page (NOP padding first if needed)."""
    start = len(code)
    body = body_fn(start)
    if (start % PAGE) + len(body) > PAGE:
        code += bytes(PAGE - start % PAGE)
        start = len(code)
        body = body_fn(start)
    code += body
    return start


def jump(op, at, target):
    return bytes([op]) + struct.pack('<h', target - (at + 3))


p = Program(sys.argv[1])
r = analyse(p)
assert len(r['get_cur']) == 1 and len(r['is_playable']) == 1, 'helpers not found once'
get_cur, is_playable, private = r['get_cur'][0], r['is_playable'][0], r['private']
native = len(p.natives)
p.natives.append(OFFICER)
officer = bytes([44, 0 << 2 | 1, native >> 8, native & 0xFF])  # NATIVE officer args=0 rets=1
code = p.code


def get_cur4(at):
    b = bytearray([45, 0, 2, 0, 0, 93]) + u24(get_cur) + bytes([42, 37, 145])  # ENTER; CALL get_cur; DUP; PUSH_U8 145
    ret = at + len(b) + 3 + 4 + 3 + 1 + 1 + 3                                     # the final LEAVE
    b += jump(87, at + len(b), ret)                                               # IEQ_JZ ret (not 145)
    b += officer
    b += jump(86, at + len(b), ret)                                               # JZ ret (not the officer)
    b += bytes([43, 116, 46, 0, 1])                                               # DROP; PUSH 3; LEAVE 0 1
    assert at + len(b) == ret
    b += bytes([46, 0, 1])                                                        # LEAVE 0 1 (returns get_cur's value)
    return bytes(b)


def is_char4(at):
    b = bytearray([45, 1, 3, 0, 0, 56, 0, 116])               # ENTER 1 3; LOCAL_U8_LOAD 0; PUSH 3
    l1 = at + len(b) + 3 + 1 + 3
    b += jump(91, at + len(b), l1)                            # ILT_JZ l1
    b += bytes([114, 46, 1, 1])                               # PUSH 1; LEAVE 1 1
    assert at + len(b) == l1
    b += bytes([56, 0, 116])                                  # LOCAL_U8_LOAD 0; PUSH 3
    l0 = at + len(b) + 3 + 4 + 3
    b += jump(87, at + len(b), l0)                            # IEQ_JZ l0
    b += officer + bytes([46, 1, 1])                          # NATIVE officer; LEAVE 1 1
    assert at + len(b) == l0
    b += bytes([113, 46, 1, 1])                               # PUSH 0; LEAVE 1 1
    return bytes(b)


size = len(code)
g4 = append(code, get_cur4)
i4 = append(code, is_char4)
moved = {get_cur: 0, is_playable: 0}
for target, new in ((get_cur, g4), (is_playable, i4)):
    for site in r['calls'].get(target, []):
        if r['owner'](site) in private:
            continue
        code[site + 1:site + 4] = u24(new)
        moved[target] += 1
open(sys.argv[2], 'wb').write(p.build())
print('%s: code %d -> %d, get_cur4 @%d (%d calls), is_char4 @%d (%d calls), officer native #%d' % (p.name, size, len(Program(sys.argv[2]).code),
      g4, moved[get_cur], i4, moved[is_playable], native))
