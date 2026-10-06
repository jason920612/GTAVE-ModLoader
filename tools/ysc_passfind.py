"""Find each mission script's own pass routine (mirror of examples/trainer/mission.cpp).
usage: ysc_passfind.py <file.ysc>..."""
import sys, bisect
sys.path.insert(0, __import__('os').path.dirname(__file__))
from ysc import Prog
from ysc_dis import native_name
import struct
JUMPS=set(range(85,93))
def decode(c):
    out=[]; ip=0
    L1={37,52,53,54,55,56,57,58,59,60,61,62,64,65,66,104,105,106,107}
    while ip<len(c):
        op=c[ip]; opd=0
        if op in L1: n=2; opd=c[ip+1]
        elif op==38: n=3
        elif op==39: n=4
        elif op in (40,41): n=5
        elif op==44: n=4; opd=(c[ip+2]<<8)|c[ip+3]
        elif op==45: n=5+c[ip+4]; opd=c[ip+1]
        elif op==46: n=3
        elif 67<=op<=72: n=3
        elif 73<=op<=84: n=3; opd=c[ip+1]|c[ip+2]<<8
        elif op in JUMPS: n=3; opd=ip+3+struct.unpack_from('<h',c,ip+1)[0]
        elif op==93 or 94<=op<=100: n=4; opd=c[ip+1]|c[ip+2]<<8|c[ip+3]<<16
        elif op==101: n=2+6*c[ip+1]; opd=c[ip+1]
        else: n=1
        out.append((ip,op,n,opd)); ip+=n
    return out
def find(path):
    p=Prog(path); c=bytes(p.code); ins=decode(c)
    term=[i for i in range(p.nnatives) if native_name(p,i)=='TERMINATE_THIS_THREAD']
    if not term: return None
    term=term[0]
    fs=[(a,opd) for a,op,n,opd in ins if op==45]; starts=[a for a,_ in fs]
    F=lambda a: bisect.bisect_right(starts,a)-1
    nf=len(fs); terminates=[False]*nf; helper=[False]*nf; callers=[[] for _ in range(nf)]
    for a,op,n,opd in ins:
        f=F(a)
        if op==44 and opd==term: terminates[f]=True
        if op in (84,99) and opd in (65082,65074) and fs[f][1]==2: helper[f]=True
        if op==93 and opd<len(c): callers[F(opd)].append((f,a))
    calls=[set() for _ in range(nf)]
    for a,op,n,opd in ins:
        if op==93 and opd<len(c): calls[F(a)].add(F(opd))
    for _ in range(3):  # reaches TERMINATE_THIS_THREAD through up to 3 calls
        terminates=[terminates[f] or any(terminates[x] for x in calls[f]) for f in range(nf)]
    addrs=[a for a,_,_,_ in ins]
    def ends_after(site):
        k=bisect.bisect_left(addrs,site)
        for step in range(400):
            if k>=len(ins): return False
            a,op,n,opd=ins[k]
            if op==44 and opd==term: return True
            if op==93 and opd<len(c) and terminates[F(opd)]: return True
            if op==85 and opd>a: k=bisect.bisect_left(addrs,opd); continue  # forward jump: follow it
            if op==85: k+=1; continue  # loop back: its exit is the next instruction
            if step and op in (85,46,101): return False
            k+=1
        return False
    level=[f for f in range(nf) if helper[f]]; seen=set(level)
    for depth in range(4):
        nxt=[]
        for callee in level:
            for caller,site in callers[callee]:
                if ends_after(site):
                    return ('main' if caller==0 else 'fn', site if caller==0 else fs[caller][0], fs[caller][1])
                if caller!=0 and caller not in seen: seen.add(caller); nxt.append(caller)
        level=nxt
        if not level: break
    return None
for f in sys.argv[1:]:
    try: print(f.split('/')[-1], find(f))
    except Exception as e: print(f.split('/')[-1], 'ERR', e)
