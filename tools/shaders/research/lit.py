"""Extract each dumped shader's literal constants (c240..c255, which the ucode dump omits).

Needs xbox_shaders.bin and xbox_preinit_shaders.bin in xsim.WORK: the raw
'xbox_shaders' (offset 0, 14619013 bytes) and 'xbox_preinit_shaders' (offset
14978726, 137549 bytes) entries of assets/gen/main_xbox_0.ark. The literal block
is the physOffset bytes right before each shader's microcode in that blob (the
container's definition table puts one float4 run ending at c255 there).
Reads the ucode dump in xsim.DUMP and writes xsim.LITS (lits.json), which
xsim.lits() reads.
"""
import re,glob,os,struct,json
from xsim import DUMP,WORK,LITS
d=open(os.path.join(WORK,'xbox_shaders.bin'),'rb').read()
p=open(os.path.join(WORK,'xbox_preinit_shaders.bin'),'rb').read()
os.chdir(DUMP)
def sw(b): return b''.join(b[i:i+4][::-1] for i in range(0,len(b),4))
out={}
for f in sorted(glob.glob('*.ucode.bin.*')):
    h=f[7:23]; kind=f.split('.')[-1]
    b=sw(open(f,'rb').read())
    txt=open(f.replace('.ucode.bin.','.ucode.'),'r').read()
    refs=set(int(x) for x in re.findall(r'\bc(\d+)',txt))
    hi=[r for r in refs if r>=240]
    best=None
    for blob,name in ((d,'main'),(p,'pre')):
        i=blob.find(b)
        if i>=0: best=(name,i,blob,len(b)); break
    if not best and kind=='vert':
        # match the longest suffix (vfetch instructions are patched at bind time)
        for k in range(len(b)-12,0,-12):
            i=d.find(b[k:]); 
            if i>=0 and d.count(b[k:])==1 and len(b)-k>=36:
                best=('main',i-k,d,len(b)); break
            if i<0: continue
    if not best: out[h]={'kind':kind,'found':False}; continue
    name,i,blob,n=best
    lits={}
    if hi:
        lo=min(hi)
        for r in range(lo,256):
            o=i-(256-r)*16
            lits[r]=struct.unpack('>4f',blob[o:o+16])
    out[h]={'kind':kind,'found':True,'where':name,'off':i,'lits':lits}
json.dump(out,open(LITS,'w'),indent=0)
for h,v in out.items():
    if v.get('lits'): print(v['kind'],h,{k:tuple(round(x,6) for x in t) for k,t in v['lits'].items()})
    elif not v['found']: print(v['kind'],h,'NOT FOUND')
