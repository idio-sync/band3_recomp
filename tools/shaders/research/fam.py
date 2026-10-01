import re, sys, itertools
from hyp import *
D=xsim.DUMP
def refs(h,kind='frag'):
    t=open(D+'/shader_%s.ucode.%s'%(h,kind)).read()
    return set(int(x) for x in re.findall(r'\bc(\d+)',t)), set(re.findall(r'tf(\d+)',t)), t
def inputs_read(h):
    # interpolator registers read before written
    prog=xsim.load(h); written=set(); read=set()
    for g in prog:
        rs=set(); ws=set()
        for ins in g:
            op,_,rest=ins.partition(' ')
            args=[a.strip() for a in rest.split(',')]
            for a in args[1:]:
                m=re.match(r'-?(?:r_abs\[(\d+)\]|r(\d+))',a)
                if m: rs.add(int(m.group(1) or m.group(2)))
            m=re.match(r'r(\d+)',args[0])
            if m: ws.add(int(m.group(1)))
        read|= (rs-written); written|=ws
    return read
def model(h,c,r,t,flags):
    N=norm(r[3]); P=r[1][:3]
    V=norm(r[2]) if 2 in flags['inputs'] else None
    lit=box(N,c)
    for i in range(flags['npt']):
        pl=point(i,P,N,c); lit=[lit[k]+pl[k] for k in range(3)]
    if flags['vc']:
        diff=[c[0][k]*lit[k]+r[4][k]*c[1][k] for k in range(3)]
    else:
        diff=[c[0][k]*(c[1][k]+lit[k]) for k in range(3)]
    tx=t.get('tf0',[1,1,1,1])
    mult=c[5][1] if flags['intens'] else 1.0
    rgb=[diff[k]*tx[k]*mult for k in range(3)]
    if flags['spec']:
        p=c[2][3]; R=refl(N,V); sb=boxspec(R,p,c); F=F_(N,V)
        sp=[sb[k]*F for k in range(3)]
        for i in range(flags['npt']):
            ps=ptspec(i,P,N,V,p,c); sp=[sp[k]+ps[k] for k in range(3)]
        sm=t.get('tf2',[1,1,1,1]) if flags['specmap'] else [1,1,1,1]
        rgb=[rgb[k]+sp[k]*c[2][k]*nrm(p) for k in range(3)]
    if flags['glow']:
        g=t['tf3']; rgb=[rgb[k]+g[k]*c[5][0] for k in range(3)]
    return rgb,tx
def classify(h):
    cs,tfs,txt=refs(h)
    ins=inputs_read(h)
    f=dict(inputs=ins, npt=(2 if 65 in cs else 1 if 64 in cs else 0), vc=(4 in ins and 2 not in ins and 0 in cs) , spec=2 in cs,
           glow='3' in tfs, intens=(5 in cs and '3' not in tfs), specmap='2' in tfs, bloom=7 in cs)
    return f,cs,tfs
if __name__=='__main__':
    hs=sys.argv[1:]
    for h in hs:
      try:
            f,cs,tfs=classify(h)
            okrgb=0; alpha={}
            for s in range(40):
                st,c,r,t=setup(h,seed=s)
                xsim.run(xsim.load(h),st); out=st.o['oC0']
                rgb,tx=model(h,c,r,t,f)
                okrgb+=close(out[:3],rgb)
                cands={'c0a*c1a':c[0][3]*c[1][3],'c0a*c1a*ta':c[0][3]*c[1][3]*tx[3],'vca*c1a*ta':r[4][3]*c[1][3]*tx[3],'vca*c1a':r[4][3]*c[1][3],
                       'dot(rgb,c7)':dot(out[:3],c[7]),'c0a*c1a*c5y':c[0][3]*c[1][3]*c[5][1],'c0a*c1a*ta*c5y':c[0][3]*c[1][3]*tx[3]*c[5][1]}
                for k,v in cands.items():
                    alpha[k]=alpha.get(k,0)+(abs(out[3]-v)<1e-4)
            best=[k for k,v in alpha.items() if v==40]
            print(h,'rgb %d/40'%okrgb,'alpha',best or max(alpha.items(),key=lambda kv:kv[1]),{k:v for k,v in f.items() if k!='inputs'},'in',sorted(f['inputs']),'tf',sorted(tfs))
      except Exception as e:
        print(h,'ERROR',repr(e)[:120])
