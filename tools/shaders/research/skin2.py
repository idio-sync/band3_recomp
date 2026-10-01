"""Generic model of the skin family (wrap diffuse, two normals, squared fresnel)."""
import sys
from hyp import *
from fam import refs
SHALL=True
WA=(0.55,0.6,0.65); WB=(0.45,0.4,0.35)
def skin(c,r,t,o):
    V=norm(r[2]); P=r[1][:3]; Nv=r[3]
    if o['nmap']:
        T=r[5]; B=r[4]
        nm=t['tf1']; nx=nm[0]*2-1; ny=nm[1]*2-1; nz=sat(1-nx*nx-ny*ny)
        N1=norm([nz*Nv[k]+c[14][0]*(nx*T[k]+ny*B[k]) for k in range(3)])
        if o['detail']:
            dm=t['tf14']; dx=dm[0]*2-1; dy=dm[1]*2-1; dz=sat(1-dx*dx-dy*dy); s_=c[106][0]
            nx+=s_*dx; ny+=s_*dy; nz+=s_*dz
        N2=norm([nz*Nv[k]+(nx*T[k]+ny*B[k]) for k in range(3)])
    else:
        N1=N2=norm(Nv)
    ao=r[o['aoreg']] if o['ao'] else [1,1,1,1]; aoA=ao[3]
    sm=t['tf2'] if o['specmap'] else [1,1,1,1]
    sc=[sm[k]*c[2][k] for k in range(4)]; p=max(sc[3],0.5) if o['specmap'] else c[2][3]
    if o['shadow']:
        shc=r[o['shadow']]; depth=shc[2]/shc[3]; lit=1.0 if t['tf5'][0]>=depth else 0.0
        sh1=[1-0.75*c[107][k]*sat(-dot(N1,c[108]))*(1-lit) for k in range(3)]
        sh2=[1-0.75*c[107][k]*sat(-dot(N2,c[108]))*(1-lit) for k in range(3)]
    else:
        sh1=sh2=[1,1,1]
    vn1=sat(dot(V,N1)); vn2=sat(dot(V,N2))
    bN=box(N1,c)
    amb=[bN[k]*aoA for k in range(3)]
    if o['rim']:
        rp=c[63][3]; F0=(1-vn1)*(0.5*N1[2]+0.5); rA=F0**rp if F0>0 else 0.0
        bV=box([-x for x in V],c); amb=[amb[k]*(1+rA*bV[k]) for k in range(3)]
    dl=[0,0,0]; rimP=[0,0,0]; spec=[0,0,0]
    R=refl(N2,V)
    for i in range(o['npt']):
        L=[c[64+i][k]-P[k] for k in range(3)]; d=math.sqrt(dot(L,L)); Ln=[x/d for x in L]
        att=sat(d*c[64+i][3]+c[67+i][3])
        s1=sh1 if (i==0 or SHALL) else [1,1,1]; s2=sh2 if (i==0 or SHALL) else [1,1,1]
        nl=dot(N1,Ln); wrap=[sat(nl*WA[k]+WB[k]) for k in range(3)]
        dl=[dl[k]+ao[i]*c[67+i][k]*att*wrap[k]*s1[k] for k in range(3)]
        if o['rim']:
            rB=(1-vn2*vn2)**c[63][3]; bl=sat(-dot(Ln,V))
            rimP=[rimP[k]+ao[i]*c[67+i][k]*bl*rB*(0.5*N2[2]+0.5)*att*s2[k] for k in range(3)]
        Rl=refl(N2,Ln); vr=sat(dot(V,Rl)); ps_=(vr**p if vr>0 else 0.0)*(1-vn2*vn2)**2
        spec=[spec[k]+ao[i]*c[67+i][k]*ps_*nrm(p)*att*s2[k] for k in range(3)]
    tx=t.get('tf0',[1,1,1,1]) if o['tex'] else [1,1,1,1]
    diff=[((dl[k]+amb[k])*c[0][k]+c[1][k]*c[0][k]*aoA)*tx[k] for k in range(3)]
    if o['rim']:
        diff=[diff[k]*(1+c[63][k]*rimP[k]) for k in range(3)]
    bs=boxspec(R,p,c); Fs=((1-vn2)*(0.5*N2[2]+0.5))**2
    spec=[spec[k]+bs[k]*Fs*nrm(p)*aoA for k in range(3)]
    return [diff[k]+sc[k]*spec[k] for k in range(3)]
def test(h,n=20):
    cs,tfs,_=refs(h)
    base=dict(nmap=14 in cs, detail='14' in tfs, rim=63 in cs, npt=(2 if 65 in cs else 1 if 64 in cs else 0), specmap='2' in tfs, tex='0' in tfs)
    runs=[]
    for s in range(n):
        st,c,r,t=setup(h,seed=s); xsim.run(xsim.load(h),st); runs.append((st.o['oC0'],c,r,t))
    best=(0,)
    for ao_ in (False,True):
        for aoreg in ((4,5,6,7,8) if ao_ else (4,)):
            for shr in ((4,5,6,7,8,9) if 107 in cs else (0,)):
                o=dict(base,ao=ao_,aoreg=aoreg,shadow=shr)
                ok=sum(close(out[:3],skin(c,r,t,o)) for out,c,r,t in runs)
                if ok>best[0]: best=(ok,ao_,aoreg,shr)
    return best,base
if __name__=='__main__':
    for h in sys.argv[1:]:
        try:
            b,base=test(h); print(h,'%d/20'%b[0],'ao@r%d'%b[2] if b[1] else '','shadow@r%d'%b[3] if b[3] else '',{k:v for k,v in base.items() if v})
        except Exception as e: print(h,'ERROR',repr(e)[:120])
