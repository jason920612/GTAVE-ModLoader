"""Disassemble a game script (.ysc) with native names (research/phase0.md §22).
usage: [CROSSMAP=<ModLoader/crossmap.txt>] ysc_dis.py <file.ysc>"""
import struct, re, sys
sys.path.insert(0, __import__('os').path.dirname(__file__))
from ysc import Prog
# public hash -> name from the SDK header; game hash -> public via crossmap
names={}
for m in re.finditer(r'inline [^(]*? ([A-Z0-9_]+)\([^)]*\) \{ return ml::Invoke<[^>]*>\(0x([0-9A-F]+)ULL', open(__import__('os').path.join(__import__('os').path.dirname(__file__), '../sdk/include/modloader/natives.hpp'), encoding='utf-8').read()):
    names[int(m.group(2),16)]=m.group(1)
game2pub={}
for l in open(__import__('os').environ.get('CROSSMAP', 'crossmap.txt')):
    a,b=l.strip().split(',')[:2]; game2pub[int(b,16)]=int(a,16)
rot=lambda v,n:((v<<n)|(v>>(64-n)))&0xffffffffffffffff if n else v
SIMPLE={0:'NOP',1:'IADD',2:'ISUB',3:'IMUL',4:'IDIV',5:'IMOD',6:'INOT',7:'INEG',8:'IEQ',9:'INE',10:'IGT',11:'IGE',12:'ILT',13:'ILE',
 14:'FADD',15:'FSUB',16:'FMUL',17:'FDIV',18:'FMOD',19:'FNEG',20:'FEQ',21:'FNE',22:'FGT',23:'FGE',24:'FLT',25:'FLE',26:'VADD',27:'VSUB',28:'VMUL',29:'VDIV',30:'VNEG',
 31:'IAND',32:'IOR',33:'IXOR',34:'I2F',35:'F2I',36:'F2V',42:'DUP',43:'DROP',47:'LOAD',48:'STORE',49:'STORE_REV',50:'LOAD_N',51:'STORE_N',63:'IOFFSET',
 102:'STRING',103:'STRINGHASH',108:'TEXT_LABEL_COPY',109:'CATCH',110:'THROW',111:'CALLINDIRECT',130:'IS_BIT_SET'}
for i in range(112,130): SIMPLE[i]='PUSH %s'%([-1,0,1,2,3,4,5,6,7,-1.0,0.0,1.0,2.0,3.0,4.0,5.0,6.0,7.0][i-112])
U8={37:'PUSH_U8',52:'ARRAY_U8',53:'ARRAY_U8_LOAD',54:'ARRAY_U8_STORE',55:'LOCAL_U8',56:'LOCAL_U8_LOAD',57:'LOCAL_U8_STORE',58:'STATIC_U8',59:'STATIC_U8_LOAD',60:'STATIC_U8_STORE',
 61:'IADD_U8',62:'IMUL_U8',64:'IOFFSET_U8',65:'IOFFSET_U8_LOAD',66:'IOFFSET_U8_STORE',104:'TEXT_LABEL_ASSIGN_STRING',105:'TEXT_LABEL_ASSIGN_INT',106:'TEXT_LABEL_APPEND_STRING',107:'TEXT_LABEL_APPEND_INT'}
S16={67:'PUSH_S16',68:'IADD_S16',69:'IMUL_S16',70:'IOFFSET_S16',71:'IOFFSET_S16_LOAD',72:'IOFFSET_S16_STORE'}
U16={73:'ARRAY_U16',74:'ARRAY_U16_LOAD',75:'ARRAY_U16_STORE',76:'LOCAL_U16',77:'LOCAL_U16_LOAD',78:'LOCAL_U16_STORE',79:'STATIC_U16',80:'STATIC_U16_LOAD',81:'STATIC_U16_STORE',
 82:'GLOBAL_U16',83:'GLOBAL_U16_LOAD',84:'GLOBAL_U16_STORE'}
JMP={85:'J',86:'JZ',87:'IEQ_JZ',88:'INE_JZ',89:'IGT_JZ',90:'IGE_JZ',91:'ILT_JZ',92:'ILE_JZ'}
U24={94:'STATIC_U24',95:'STATIC_U24_LOAD',96:'STATIC_U24_STORE',97:'GLOBAL_U24',98:'GLOBAL_U24_LOAD',99:'GLOBAL_U24_STORE',100:'PUSH_U24'}
def native_name(p,i):
    g=rot(p.natives[i],(p.codesize+i)&63); pub=game2pub.get(g,g)
    return names.get(pub, '0x%016X'%pub)
def disasm(p):
    c=p.code; ip=0; out=[]
    while ip<len(c):
        op=c[ip]; a=ip
        if op in SIMPLE: t=SIMPLE[op]; ip+=1
        elif op in U8: t=f'{U8[op]} {c[ip+1]}'; ip+=2
        elif op==38: t=f'PUSH {c[ip+1]} {c[ip+2]}'; ip+=3
        elif op==39: t=f'PUSH {c[ip+1]} {c[ip+2]} {c[ip+3]}'; ip+=4
        elif op==40: t=f'PUSH {struct.unpack_from("<i",c,ip+1)[0]}'; ip+=5
        elif op==41: t=f'PUSHF {struct.unpack_from("<f",c,ip+1)[0]:g}'; ip+=5
        elif op==44:
            b=c[ip+1]; idx=(c[ip+2]<<8)|c[ip+3]; t=f'NATIVE {native_name(p,idx)} args={b>>2} rets={b&3}'; ip+=4
        elif op==45:
            n=c[ip+4]; nm=c[ip+5:ip+5+n].split(b'\0')[0].decode('latin1'); t=f'ENTER params={c[ip+1]} locals={struct.unpack_from("<H",c,ip+2)[0]} {nm}'; ip+=5+n
        elif op==46: t=f'LEAVE params={c[ip+1]} rets={c[ip+2]}'; ip+=3
        elif op in S16: t=f'{S16[op]} {struct.unpack_from("<h",c,ip+1)[0]}'; ip+=3
        elif op in U16:
            v=struct.unpack_from("<H",c,ip+1)[0]; t=f'{U16[op]} {v}'; ip+=3
        elif op in JMP: off=struct.unpack_from("<h",c,ip+1)[0]; t=f'{JMP[op]} @{ip+3+off}'; ip+=3
        elif op==93: v=c[ip+1]|c[ip+2]<<8|c[ip+3]<<16; t=f'CALL @{v}'; ip+=4
        elif op in U24: v=c[ip+1]|c[ip+2]<<8|c[ip+3]<<16; t=f'{U24[op]} {v}'; ip+=4
        elif op==101:
            n=c[ip+1]; cases=[]
            for k in range(n):
                val,off=struct.unpack_from('<ih',c,ip+2+6*k); cases.append(f'{val}:@{ip+2+6*(k+1)+off}')
            t='SWITCH '+' '.join(cases); ip+=2+6*n
        else: t=f'?? {op}'; ip+=1
        out.append((a,t))
    return out
def string_at(p,off): return p.strings[off:].split(b'\0')[0].decode('latin1')
if __name__=="__main__":
    p=Prog(sys.argv[1])
    lines=disasm(p)
    # annotate string pushes: PUSH x ; STRING
    for i,(a,t) in enumerate(lines):
        if t=='STRING' and i>0:
            prev=lines[i-1][1].split()
            try: t+=' "%s"'%string_at(p,int(prev[-1]))
            except Exception: pass
        print(f'{a:6d}: {t}')
