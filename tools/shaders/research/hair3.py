"""Generic model of the c19 (specular2) family: wrap diffuse, tangent-strand two-colour spec."""
import sys
from hyp import *
from fam import refs
def hair(c,r,t,o):
    V=norm(r[2]); P=r[1][:3]; Nv=r[3]; T=r[5]; B=r[4]
    nm=t['tf1']; nx=nm[0]*2-1; ny=nm[1]*2-1; nz=sat(1-nx*nx-ny*ny)
    if o['detail']:
        dm=t['tf14']; dx=dm[0]*2-1; dy=dm[1]*2-1; dz=sat(1-dx*dx-dy*dy); s_=c[106][0]
        nx+=s_*dx; ny+=s_*dy; nz+=s_*dz
    N=norm([nz*Nv[k]+c[14][0]*(nx*T[k]+ny*B[k]) for k in range(3)])
    ao=r[o['aoreg']] if o['ao'] else [1,1,1,1]; aoA=ao[3]
    sm=t['tf2']; p=max(sm[3]*c[2][3],0.5)
    if o['shadow']:
        shc=r[o['shadow']]; depth=shc[2]/shc[3]; lit=1.0 if t['tf5'][0]>=depth else 0.0
        sh=[1-0.75*c[107][k]*sat(-dot(N,c[108]))*(1-lit) for k in range(3)]
    else: sh=[1,1,1]
    vn=sat(dot(V,N)); F=(1-vn)*(0.5*N[2]+0.5)+0.25
    R=refl(N,V); bs=boxspec(R,p,c); bx=box(N,c)
    rt=dot(R,T); s2=abs(1-rt*rt)
    A2=s2**(2*p) if s2>0 else 0.0; A4=s2**(4*p) if s2>0 else 0.0
    col=[c[2][k]*A4+c[19][k]*A2*(1-A4) for k in range(3)]
    dl=[0,0,0]; ps=[0,0,0]
    for i in range(o['npt']):
        L=[c[64+i][k]-P[k] for k in range(3)]; d=math.sqrt(dot(L,L)); Ln=[x/d for x in L]
        att=sat(d*c[64+i][3]+c[67+i][3])
        wrap=[sat(dot(N,Ln)*a+b) for a,b in zip((0.75,0.8,0.85),(0.25,0.2,0.15))]
        lr=sat(dot(Ln,R))
        dl=[dl[k]+ao[i]*c[67+i][k]*att*wrap[k]*sh[k] for k in range(3)]
        ps=[ps[k]+ao[i]*c[67+i][k]*att*sh[k]*nrm(p)*lr*col[k] for k in range(3)]
    diff=[(dl[k]+bx[k]*aoA)*c[0][k]+c[1][k]*c[0][k]*aoA for k in range(3)]
    spec=[(ps[k]+bs[k]*F*nrm(p)*aoA)*sm[k] for k in range(3)]
    return [diff[k]*t['tf0'][k]+spec[k] for k in range(3)]
if __name__=='__main__':
    for h in sys.argv[1:]:
        cs,tfs,_=refs(h)
        base=dict(detail='14' in tfs, npt=(2 if 65 in cs else 1 if 64 in cs else 0))
        runs=[]
        for s in range(20):
            st,c,r,t=setup(h,seed=s); xsim.run(xsim.load(h),st); runs.append((st.o['oC0'],c,r,t))
        best=(0,)
        for ao_ in (False,True):
            for aoreg in ((4,5,6,7,8) if ao_ else (4,)):
                for shr in ((4,5,6,7,8,9) if 107 in cs else (0,)):
                    o=dict(base,ao=ao_,aoreg=aoreg,shadow=shr)
                    ok=sum(close(out[:3],hair(c,r,t,o)) for out,c,r,t in runs)
                    if ok>best[0]: best=(ok,ao_,aoreg,shr)
        print(h,'%d/20'%best[0],best,base)
