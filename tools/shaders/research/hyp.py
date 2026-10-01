import random, math, xsim
def rv(lo=-1,hi=1): return [random.uniform(lo,hi) for _ in range(4)]
def norm(v): l=math.sqrt(sum(x*x for x in v[:3])); return [x/l for x in v[:3]]
def dot(a,b): return sum(a[i]*b[i] for i in range(3))
def sat(x): return min(max(x,0.0),1.0)
def setup(h, kind='frag', seed=None, consts=None, regs=None, tex=None):
    if seed is not None: random.seed(seed)
    c={i:rv(0,1) for i in range(0,256)}
    c.update(xsim.lits(h))
    if consts: c.update(consts)
    r={i:rv() for i in range(16)}
    if regs: r.update(regs)
    t={}
    def fetch(op,unit,coord,line):
        if unit not in t: t[unit]=(tex or {}).get(unit) or rv(0,1)
        return t[unit]
    st=xsim.State(c,r,fetch)
    return st,c,r,t
def box(N,c):
    out=[0,0,0]
    for a in range(3):
        p=max(N[a],0); n=max(-N[a],0)
        for k in range(3): out[k]+=p*c[80+2*a][k]+n*c[81+2*a][k]
    return out
def point(i,P,N,c):
    L=[c[64+i][k]-P[k] for k in range(3)]; d=math.sqrt(dot(L,L)); Ln=[x/d for x in L]
    att=sat(d*c[64+i][3]+c[67+i][3]); nl=sat(dot(N,Ln))
    return [c[67+i][k]*att*nl for k in range(3)]
def close(a,b,tol=1e-4): return all(abs(x-y)<=tol*max(1,abs(y)) for x,y in zip(a,b))
def refl(N,V):
    d=dot(N,V); return [2*d*N[k]-V[k] for k in range(3)]
def boxspec(R,p,c):
    out=[0,0,0]
    for a in range(3):
        pp=sat(R[a])**p if R[a]>0 else 0.0; nn=sat(-R[a])**p if R[a]<0 else 0.0
        for k in range(3): out[k]+=pp*c[80+2*a][k]+nn*c[81+2*a][k]
    return out
def ptspec(i,P,N,V,p,c,with_nl=False):
    L=[c[64+i][k]-P[k] for k in range(3)]; d=math.sqrt(dot(L,L)); Ln=[x/d for x in L]
    att=sat(d*c[64+i][3]+c[67+i][3]); R=refl(N,V)
    rl=sat(dot(R,Ln)); s=rl**p if rl>0 else 0.0
    if with_nl: s*=sat(dot(N,Ln))
    return [c[67+i][k]*att*s for k in range(3)]
def F_(N,V): return (1-sat(dot(V,N)))*(0.5*N[2]+0.5)+0.25
def nrm(p): return (p+2)/(2*math.pi)
