"""Runs functions of a game script (.ysc) offline, to read the data tables they hold (research/phase0.md §28).

usage: ysc_eval.py <file.ysc> <function address> [args...]      -> return values and what was written through pointers
       (args: integers, floats with a '.', or 'out<N>' for a pointer to N zeroed slots)
As a module: Vm(path).call(address, args) -> (returned values, out buffers).

Covers the bytecode a data function uses (arithmetic, locals, statics, pointers, switch, calls, text labels). Globals
read as 0 unless given; natives other than a few pure ones stop the run (Unsupported)."""
import math, struct, sys, os
sys.path.insert(0, os.path.dirname(__file__))
from ysc import Prog


class Unsupported(Exception):
    pass


def f32(v):
    return struct.unpack("<f", struct.pack("<f", v))[0]


class Ptr:
    """A pointer to a slot: area is 'stack', 'static', 'global' or a list (an out buffer); index in slots."""
    __slots__ = ("area", "index")

    def __init__(self, area, index):
        self.area, self.index = area, index

    def __add__(self, n):
        return Ptr(self.area, self.index + n)

    def __repr__(self):
        return f"&{self.area if isinstance(self.area, str) else 'buf'}[{self.index}]"


class Vm:
    def __init__(self, path, globals_=None, natives=None):
        self.p = Prog(path)
        self.code = bytes(self.p.code)
        self.statics = list(self.p.statics)
        self.globals = dict(globals_ or {})
        self.natives = natives or {}
        self.names = {}
        try:  # native names from the SDK, for messages and the pure ones below
            import ysc_dis
            rot = lambda v, n: ((v << n) | (v >> (64 - n))) & 0xffffffffffffffff if n else v
            for i, h in enumerate(self.p.natives):
                g = rot(h, (self.p.codesize + i) & 63)
                pub = ysc_dis.game2pub.get(g, g)
                self.names[i] = ysc_dis.names.get(pub, f"0x{pub:016X}")
        except Exception:
            pass

    # --- memory ------------------------------------------------------------------------------------------------------
    def load(self, ptr):
        if not isinstance(ptr, Ptr):
            raise Unsupported(f"load from {ptr!r}")
        a = ptr.area
        if a == "stack":
            return self.stack[ptr.index]
        if a == "static":
            return self.statics[ptr.index]
        if a == "global":
            return self.globals.get(ptr.index, 0)
        return a[ptr.index]

    def store(self, ptr, value):
        if not isinstance(ptr, Ptr):
            raise Unsupported(f"store to {ptr!r}")
        a = ptr.area
        if a == "stack":
            while len(self.stack) <= ptr.index:
                self.stack.append(0)
            self.stack[ptr.index] = value
        elif a == "static":
            self.statics[ptr.index] = value
        elif a == "global":
            self.globals[ptr.index] = value
        else:
            while len(a) <= ptr.index:
                a.append(0)
            a[ptr.index] = value

    def string(self, offset):
        return self.p.strings[offset:].split(b"\0")[0].decode("latin1")

    # --- run ---------------------------------------------------------------------------------------------------------
    def call(self, address, args=(), limit=5_000_000):
        self.stack = []
        outs = []
        for a in args:
            if isinstance(a, str) and a.startswith("out"):
                buf = [0] * int(a[3:])
                outs.append(buf)
                self.stack.append(Ptr(buf, 0))
            else:
                self.stack.append(a)
        self.stack.append(-1)  # return address of the outermost call
        return self.run(address, limit), outs

    def run(self, ip, limit):
        c = self.code
        st = self.stack
        pop = st.pop
        push = st.append
        fp = 0
        frames = []
        u8 = lambda: c[ip + 1]
        s16 = lambda: struct.unpack_from("<h", c, ip + 1)[0]
        u16 = lambda: struct.unpack_from("<H", c, ip + 1)[0]
        u24 = lambda: c[ip + 1] | c[ip + 2] << 8 | c[ip + 3] << 16
        for _ in range(limit):
            op = c[ip]
            if op == 0:
                ip += 1
            elif 1 <= op <= 5:
                b, a = pop(), pop()
                if op == 1: push(a + b if not isinstance(a, Ptr) else a + b)
                elif op == 2: push(a - b)
                elif op == 3: push(a * b)
                elif op == 4: push(int(a / b) if b else 0)
                else: push(int(math.fmod(a, b)) if b else 0)
                ip += 1
            elif op == 6: push(int(not pop())); ip += 1
            elif op == 7: push(-pop()); ip += 1
            elif 8 <= op <= 13 or 20 <= op <= 25:
                b, a = pop(), pop()
                k = (op - 8) if op <= 13 else (op - 20)
                push(int([a == b, a != b, a > b, a >= b, a < b, a <= b][k])); ip += 1
            elif 14 <= op <= 18:
                b, a = pop(), pop()
                push(f32([a + b, a - b, a * b, a / b if b else 0.0, math.fmod(a, b) if b else 0.0][op - 14])); ip += 1
            elif op == 19: push(-pop()); ip += 1
            elif 26 <= op <= 29:
                b = [pop(), pop(), pop()][::-1]; a = [pop(), pop(), pop()][::-1]
                f = [lambda x, y: x + y, lambda x, y: x - y, lambda x, y: x * y, lambda x, y: x / y if y else 0.0][op - 26]
                for k in range(3): push(f32(f(a[k], b[k])))
                ip += 1
            elif op == 30:
                v = [pop(), pop(), pop()][::-1]
                for k in range(3): push(-v[k])
                ip += 1
            elif 31 <= op <= 33:
                b, a = pop(), pop()
                push([a & b, a | b, a ^ b][op - 31]); ip += 1
            elif op == 34: push(f32(float(pop()))); ip += 1
            elif op == 35: push(int(pop())); ip += 1
            elif op == 36:
                v = pop(); push(v); push(v); push(v); ip += 1
            elif op == 37: push(u8()); ip += 2
            elif op == 38: push(c[ip + 1]); push(c[ip + 2]); ip += 3
            elif op == 39: push(c[ip + 1]); push(c[ip + 2]); push(c[ip + 3]); ip += 4
            elif op == 40: push(struct.unpack_from("<i", c, ip + 1)[0]); ip += 5
            elif op == 41: push(f32(struct.unpack_from("<f", c, ip + 1)[0])); ip += 5
            elif op == 42: v = pop(); push(v); push(v); ip += 1
            elif op == 43: pop(); ip += 1
            elif op == 44:
                b = c[ip + 1]; idx = (c[ip + 2] << 8) | c[ip + 3]
                nargs, nrets = b >> 2, b & 3
                args = [pop() for _ in range(nargs)][::-1]
                name = self.names.get(idx, f"native#{idx}")
                rets = self.native(name, args)
                for k in range(nrets): push(rets[k] if k < len(rets) else 0)
                ip += 4
            elif op == 45:
                params, nlocals = c[ip + 1], struct.unpack_from("<H", c, ip + 2)[0]
                frames.append(fp)
                fp = len(st) - params - 1
                # params .. return address .. (saved fp) .. locals
                while len(st) < fp + nlocals + 2:
                    push(0)
                ip += 5 + c[ip + 4]
            elif op == 46:
                params, nrets = c[ip + 1], c[ip + 2]
                rets = st[len(st) - nrets:] if nrets else []
                ret = st[fp + params]
                del st[fp:]
                st.extend(rets)
                fp = frames.pop() if frames else 0
                if ret == -1:
                    return rets
                ip = ret
            elif op == 47: push(self.load(pop())); ip += 1
            elif op == 48:
                ptr = pop(); v = pop(); self.store(ptr, v); ip += 1
            elif op == 49:
                v = pop(); ptr = st[-1]; self.store(ptr, v); ip += 1
            elif op == 50:
                ptr = pop(); n = pop()
                for k in range(n): push(self.load(ptr + k))
                ip += 1
            elif op == 51:
                ptr = pop(); n = pop()
                vals = [pop() for _ in range(n)][::-1]
                for k in range(n): self.store(ptr + k, vals[k])
                ip += 1
            elif 52 <= op <= 54 or 73 <= op <= 75:
                size = u8() if op <= 54 else u16()
                kind = (op - 52) if op <= 54 else (op - 73)
                arr = pop(); index = pop()
                ptr = arr + (1 + index * size)
                if kind == 0: push(ptr)
                elif kind == 1: push(self.load(ptr))
                else: self.store(ptr, pop())
                ip += 2 if op <= 54 else 3
            elif 55 <= op <= 57 or 76 <= op <= 78:
                n = u8() if op <= 57 else u16()
                kind = (op - 55) if op <= 57 else (op - 76)
                ptr = Ptr("stack", fp + n)
                if kind == 0: push(ptr)
                elif kind == 1: push(self.load(ptr))
                else: self.store(ptr, pop())
                ip += 2 if op <= 57 else 3
            elif 58 <= op <= 60 or 79 <= op <= 81 or 94 <= op <= 96:
                n = u8() if op <= 60 else u16() if op <= 81 else u24()
                kind = (op - 58) if op <= 60 else (op - 79) if op <= 81 else (op - 94)
                ptr = Ptr("static", n)
                if kind == 0: push(ptr)
                elif kind == 1: push(self.load(ptr))
                else: self.store(ptr, pop())
                ip += 2 if op <= 60 else 3 if op <= 81 else 4
            elif op == 61: push(pop() + u8()); ip += 2
            elif op == 62: push(pop() * u8()); ip += 2
            elif op == 63:
                off = pop(); ptr = pop(); push(ptr + off); ip += 1
            elif 64 <= op <= 66 or 70 <= op <= 72:
                n = u8() if op <= 66 else s16()
                kind = (op - 64) if op <= 66 else (op - 70)
                ptr = pop() + n
                if kind == 0: push(ptr)
                elif kind == 1: push(self.load(ptr))
                else: self.store(ptr, pop())
                ip += 2 if op <= 66 else 3
            elif op == 67: push(s16()); ip += 3
            elif op == 68: push(pop() + s16()); ip += 3
            elif op == 69: push(pop() * s16()); ip += 3
            elif 82 <= op <= 84 or 97 <= op <= 99:
                n = u16() if op <= 84 else u24()
                kind = (op - 82) if op <= 84 else (op - 97)
                ptr = Ptr("global", n)
                if kind == 0: push(ptr)
                elif kind == 1: push(self.load(ptr))
                else: self.store(ptr, pop())
                ip += 3 if op <= 84 else 4
            elif op == 85: ip = ip + 3 + s16()
            elif op == 86: ip = ip + 3 + s16() if not pop() else ip + 3
            elif 87 <= op <= 92:
                b, a = pop(), pop()
                ok = [a == b, a != b, a > b, a >= b, a < b, a <= b][op - 87]
                ip = ip + 3 if ok else ip + 3 + s16()
            elif op == 93:
                push(ip + 4); ip = u24()
            elif op == 100: push(u24()); ip += 4
            elif op == 101:
                n = c[ip + 1]; v = pop(); target = ip + 2 + 6 * n
                for k in range(n):
                    val, off = struct.unpack_from("<ih", c, ip + 2 + 6 * k)
                    if val == v:
                        target = ip + 2 + 6 * (k + 1) + off
                        break
                ip = target
            elif op == 102: push(("str", self.string(pop()))); ip += 1
            elif op == 103:
                s = pop(); push(joaat(s[1] if isinstance(s, tuple) else str(s))); ip += 1
            elif 104 <= op <= 107:
                size = u8(); dest = pop(); src = pop()
                text = src[1] if isinstance(src, tuple) else str(src)
                cur = self.load(dest)
                cur = cur[1] if isinstance(cur, tuple) else ""
                new = text if op in (104, 105) else cur + text
                self.store(dest, ("str", new[:size - 1])); ip += 2
            elif op == 108:
                dest = pop(); count = pop(); src = pop()
                self.store(dest, self.load(src) if isinstance(src, Ptr) else src); ip += 1
            elif 112 <= op <= 129:
                push([-1, 0, 1, 2, 3, 4, 5, 6, 7, -1.0, 0.0, 1.0, 2.0, 3.0, 4.0, 5.0, 6.0, 7.0][op - 112]); ip += 1
            elif op == 130:
                b, a = pop(), pop(); push(int((a >> b) & 1)); ip += 1
            else:
                raise Unsupported(f"opcode {op} at {ip}")
        raise Unsupported("too many instructions")

    def native(self, name, args):
        if name in self.natives:
            r = self.natives[name](*args)
            return r if isinstance(r, (list, tuple)) else [r]
        if name == "GET_HASH_KEY":
            return [joaat(args[0][1] if isinstance(args[0], tuple) else str(args[0]))]
        if name == "TO_FLOAT":
            return [f32(float(args[0]))]
        if name in ("FLOOR", "ROUND", "CEIL"):
            return [int({"FLOOR": math.floor, "ROUND": round, "CEIL": math.ceil}[name](args[0]))]
        if name in ("VMAG", "VDIST"):
            raise Unsupported(name)
        raise Unsupported(f"native {name}")


def joaat(s):
    h = 0
    for ch in s.lower().encode():
        h = (h + ch) & 0xffffffff
        h = (h + (h << 10)) & 0xffffffff
        h ^= h >> 6
    h = (h + (h << 3)) & 0xffffffff
    h ^= h >> 11
    h = (h + (h << 15)) & 0xffffffff
    return h - (1 << 32) if h & 0x80000000 else h


def parse(a):
    if a.startswith("out"):
        return a
    return float(a) if "." in a else int(a, 0)


if __name__ == "__main__":
    vm = Vm(sys.argv[1])
    rets, outs = vm.call(int(sys.argv[2]), [parse(a) for a in sys.argv[3:]])
    print("returned", rets)
    for i, o in enumerate(outs):
        print("out", i, o)
