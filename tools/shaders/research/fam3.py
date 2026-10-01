"""Clean standard-material model, checked against the dumped shaders with xsim."""
import re, sys
from hyp import *
from fam import refs

def model(c, r, t, o):
    P = r[1][:3]; V = norm(r[2])
    N = norm(r[3])
    if o['nmap']:
        nm = t['tf1']; nx = nm[0]*2-1; ny = nm[1]*2-1; nz = sat(1-nx*nx-ny*ny)
        if o['detail']:
            dm = t['tf14']; dx = dm[0]*2-1; dy = dm[1]*2-1; dz = sat(1-dx*dx-dy*dy); s_ = c[106][0]
            nx += s_*dx; ny += s_*dy; nz += s_*dz
        N = norm([nz*r[3][k] + c[14][0]*(nx*r[5][k] + ny*r[4][k]) for k in range(3)])
    ao = r[o['aoreg']] if o['col'] == 'ao' else [1, 1, 1, 1]
    aoA = ao[3]
    # projected light (multiply/shadow form)
    pm = [1, 1, 1]
    if o['proj']:
        g = t['tf5'][3]; nl = sat(dot(N, c[66]))
        pm = [1-0.75*c[69][k]*g*nl for k in range(3)]
    # shadow buffer
    sh = [1, 1, 1]
    if o['shadow']:
        sc = r[o['shadow']]; depth = sc[2]/sc[3]
        lit = 1.0 if t['tf5'][0] >= depth else 0.0
        nls = sat(-dot(N, c[108]))
        sh = [1-0.75*c[107][k]*nls*(1-lit) for k in range(3)]
    lights = []
    for i in range(o['npt']):
        L = [c[64+i][k]-P[k] for k in range(3)]; d = math.sqrt(dot(L, L)); Ln = [x/d for x in L]
        att = sat(d*c[64+i][3] + c[67+i][3])
        k_sh = sh if (i == 0 or o['shall']) else [1, 1, 1]
        w = [ao[i]*k_sh[k]*pm[k]*att for k in range(3)]   # per-light weight (rgb)
        lights.append((i, Ln, w))
    bx = [box(N, c)[k]*pm[k] for k in range(3)]
    dl = [0, 0, 0]
    for i, Ln, w in lights:
        nl = sat(dot(N, Ln)); dl = [dl[k] + c[67+i][k]*w[k]*nl for k in range(3)]
    if o['col'] == 'vc':
        diff = [c[0][k]*(bx[k]+dl[k]) + r[4][k]*c[1][k] for k in range(3)]
    else:
        diff = [c[0][k]*(aoA*(c[1][k]+bx[k]) + dl[k]) for k in range(3)]
    tx = t.get('tf0', [1, 1, 1, 1]) if o['tex'] else [1, 1, 1, 1]
    base_ = [tx[k]*(c[5][1] if o['intens'] else 1.0) for k in range(3)]
    if o['env']:
        E = t['tf4']; ew = E[3]*((1-sat(dot(N, V))) if o['env'] == 'fres' else 1.0)
        base_ = [base_[k] + E[k]*ew for k in range(3)]
    rgb = [diff[k]*base_[k] for k in range(3)]
    dtex = list(rgb)
    if o['spec']:
        sm = t['tf2'] if o['specmap'] else None
        p = max(sm[3]*c[2][3], 0.5) if sm else c[2][3]
        R = refl(N, V); bs = boxspec(R, p, c); F = F_(N, V)
        sp = [bs[k]*F*aoA*pm[k]*nrm(p) for k in range(3)]
        for i, Ln, w in lights:
            if o['hair']:
                H = [Ln[k]+V[k] for k in range(3)]; nh = dot(N, H); Ht = norm([H[k]-N[k]*nh for k in range(3)])
                T = norm([-N[1], N[0], 0.0]); kk = dot(Ht, T); pe = p + (c[13][0]-p)*kk*kk
                vr = sat(dot(V, refl(N, Ln))); s_ = vr**pe if vr > 0 else 0.0
            else:
                rl = sat(dot(R, Ln)); s_ = (rl**p if rl > 0 else 0.0)*nrm(p)
            sp = [sp[k] + c[67+i][k]*w[k]*s_ for k in range(3)]
        sm = sm or [1, 1, 1, 1]
        rgb = [rgb[k] + sp[k]*c[2][k]*sm[k] for k in range(3)]
    if o['rim']:
        rmap = t['tf15'] if o['rimmap'] else [1, 1, 1, 1]
        rp = max(c[63][3]*rmap[3], 0.5) if o['rimmap'] else c[63][3]
        vn = sat(dot(V, N))
        rA = ((1-vn)*(0.5*N[2]+0.5))**rp; rB = (1-vn*vn)**rp
        bv = box([-x for x in V], c)
        rim = [rA*aoA*bv[k]*pm[k] for k in range(3)]
        for i, Ln, w in lights:
            bl = sat(-dot(Ln, V)); bz = (0.5*N[2]+0.5) if o['rimnz'] else 1.0
            rim = [rim[k] + c[67+i][k]*w[k]*bl*rB*bz for k in range(3)]
        rim = [rim[k]*c[63][k]*rmap[k] for k in range(3)]
        rgb = [rgb[k] + dtex[k]*0.3*rim[k] + 0.7*rim[k] for k in range(3)]
    if o['glow']:
        g = t['tf3']; rgb = [rgb[k] + g[k]*c[5][0] for k in range(3)]
    return rgb, tx

def search(h, n=30):
    cs, tfs, txt = refs(h)
    base = dict(detail='14' in tfs, rim=63 in cs, rimmap='15' in tfs, hair=13 in cs, nmap=14 in cs, proj=95 in cs,
                npt=(2 if 65 in cs else 1 if 64 in cs else 0), spec=2 in cs, glow='3' in tfs,
                intens=bool(re.search(r'c5.y', txt)), tex='0' in tfs, specmap='2' in tfs)
    runs = []
    for s in range(n):
        st, c, r, t = setup(h, seed=s)
        xsim.run(xsim.load(h), st); runs.append((st.o['oC0'], c, r, t))
    hits = []
    for col in ('none', 'vc', 'ao'):
        for aoreg in ((4, 5, 6, 7, 8) if col == 'ao' else (4,)):
            for shreg in ((4, 5, 6, 7, 8, 9) if 107 in cs else (0,)):
                for shall in ((False, True) if (107 in cs and base['npt'] > 1) else (False,)):
                    for env in (('add', 'fres') if '4' in tfs else (None,)):
                        for rimnz in ((False, True) if base['rim'] else (False,)):
                            o = dict(base, col=col, aoreg=aoreg, shadow=shreg, shall=shall, env=env, rimnz=rimnz)
                            ok = sum(close(out[:3], model(c, r, t, o)[0]) for out, c, r, t in runs)
                            hits.append((ok, col, aoreg, shreg, shall, env or '', rimnz))
    hits.sort(key=lambda x: x[0], reverse=True)
    best = hits[0]
    o = dict(base, col=best[1], aoreg=best[2], shadow=best[3], shall=best[4], env=best[5] or None, rimnz=best[6])
    al = {}
    for out, c, r, t in runs:
        rgb, tx = model(c, r, t, o)
        aoA = r[o['aoreg']][3] if o['col'] == 'ao' else 1.0
        cands = {'c0a*c1a': c[0][3]*c[1][3], 'c0a*c1a*ta': c[0][3]*c[1][3]*tx[3], 'vca*c1a*ta': r[4][3]*c[1][3]*tx[3],
                 'dot(rgb,c7)': dot(out[:3], c[7]), 'c0a*c1a*ta*c5y': c[0][3]*c[1][3]*tx[3]*c[5][1],
                 'c0a*c1a*aoW*ta': c[0][3]*c[1][3]*aoA*tx[3]}
        for k, v in cands.items(): al[k] = al.get(k, 0) + (abs(out[3]-v) < 1e-4)
    alpha = [k for k, v in al.items() if v == n]
    return best, o, alpha, cs, tfs

if __name__ == '__main__':
    for h in sys.argv[1:]:
        try:
            best, o, alpha, cs, tfs = search(h)
            extra = sorted(x for x in cs if x not in (0, 1, 2, 5, 7, 64, 65, 67, 68, 80, 81, 82, 83, 84, 85) and x < 240)
            flags = ' '.join(k for k in ('nmap', 'detail', 'spec', 'specmap', 'hair', 'rim', 'rimmap', 'proj', 'glow', 'intens', 'tex') if o[k])
            print(h, '%2d/30' % best[0], 'col=%s%s' % (o['col'], '@r%d' % o['aoreg'] if o['col'] == 'ao' else ''),
                  'shadow@r%d%s' % (o['shadow'], '(all)' if o['shall'] else '') if o['shadow'] else '', 'env=%s' % o['env'] if o['env'] else '',
                  'rimNz' if o['rimnz'] else '', 'npt=%d' % o['npt'], flags, 'alpha', alpha, 'tf', sorted(tfs), 'extra', extra)
        except Exception as e:
            print(h, 'ERROR', repr(e)[:150])
