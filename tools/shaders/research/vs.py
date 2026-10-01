from hyp import *
import re
def vsetup(h,seed):
    random.seed(seed)
    c={i:rv(0,1) for i in range(256)}
    c.update(xsim.lits(h))
    attrs={}
    def fetch(op,unit,coord,line):
        m=re.search(r'Offset=(\d+)',line); off=int(m.group(1)) if m else 0
        if off not in attrs: attrs[off]=rv(-1,1) if 'FMT_2_10_10_10' in line or 'FLOAT' in line else rv(0,1)
        return attrs[off]
    st=xsim.State(c,{0:[0,0,0,0]},fetch)
    return st,c,attrs
def xf(c,v,w=1.0):
    return [c[92+i][0]*v[0]+c[92+i][1]*v[1]+c[92+i][2]*v[2]+c[92+i][3]*w for i in range(3)]
