"""Same as lit.py for vertex shaders: their vfetch instructions are patched at bind
time, so the microcode is located by its ALU tail instead. Run after lit.py; it
updates the same lits.json (xsim.LITS).
"""
import re,glob,os,struct,json
from xsim import DUMP,WORK,LITS
d=open(os.path.join(WORK,'xbox_shaders.bin'),'rb').read()
p=open(os.path.join(WORK,'xbox_preinit_shaders.bin'),'rb').read()
os.chdir(DUMP)
def sw(b): return b''.join(b[i:i+4][::-1] for i in range(0,len(b),4))
res=json.load(open(LITS))
for f in sorted(glob.glob('*.ucode.bin.vert')):
    h=f[7:23]
    b=sw(open(f,'rb').read())
    n=len(b)//12
    while n and b[(n-1)*12:n*12]==bytes(12): n-=1
    txt=open(f.replace('.ucode.bin.','.ucode.'),'r').read()
    hi=[int(x) for x in set(re.findall(r'\bc(\d+)',txt)) if int(x)>=240]
    # locate: the last patched (absent) chunk, then match the rest
    zs=[k for k in range(n) if d.count(b[k*12:k*12+12])==0 and p.count(b[k*12:k*12+12])==0]
    st=(max(zs)+1) if zs else 0
    tail=b[st*12:n*12]
    hit=None
    for blob,name in ((d,'main'),(p,'pre')):
        c=blob.count(tail)
        if c:
            hit=(name,blob.find(tail)-st*12,blob,c); break
    if not hit:
        print('vert',h,'NOT FOUND',zs); res[h]={'kind':'vert','found':False}; continue
    name,i,blob,c=hit
    lits={}
    if hi:
        for r in range(min(hi),256):
            o=i-(256-r)*16
            lits[r]=struct.unpack('>4f',blob[o:o+16])
    res[h]={'kind':'vert','found':True,'where':name,'off':i,'lits':lits,'dups':c}
    print('vert',h,name,'dups',c,{k:tuple(round(x,6) for x in t) for k,t in lits.items()})
json.dump(res,open(LITS,'w'),indent=0)
