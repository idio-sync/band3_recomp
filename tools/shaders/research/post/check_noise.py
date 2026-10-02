"""Check the noise (film grain) term of RB3's composite against a Python model.

NgPostProc::CheckNoise sets c112 (four random seeds each frame, or the two
stationary ones twice), c113 (base scale x, y, top scale or 1, intensity) and
binds the noise map to sampler 13, linear and wrapping. The composite reads it
twice, at (uv + c112.xy) * c113.xy and (uv + c112.zw) * c113.xy * c113.z, takes
the taps' geometric mean n per channel and overlays it on the colour by its
luminance L (2 n rgb at L <= 0.5, else 1 - 2 (1 - n)(1 - rgb)), moving the
colour that way by 6.75 c113.w L (1 - L)^2; after DOF, bloom or glare and the
spotlights' term, before the colour matrix, unsaturated until the end
(out/research/n1_post_noise.md). src/Render/shaders/post_model.hlsli's
NoiseTerm is this model, and tests/post_model_test.cpp checks it there.

Runs the three straight-line composite variants with the noise in xsim, with
random constants (c112 in 0..1, c113.xy 1..40, top 0.5..2, intensity -3..3),
texture contents and uv, and compares oC0 and every fetch's (sampler,
coordinate) with the model. neg_noise.py's wrong models must fail it.

Usage: python check_noise.py [trials]   (default 200)
"""
import math
import os
import random
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), '..'))
import xsim
from check_post import Tex, run_shader, rv, sat, consts_composite, dof_amount


def noise_taps(c, uv):
    u, v = uv
    s, p = c[112], c[113]
    a = ((u + s[0]) * p[0], (v + s[1]) * p[1])
    b = ((u + s[2]) * p[0] * p[2], (v + s[3]) * p[1] * p[2])
    return a, b


def noise(c, uv, T, rgb, midtone_const=6.75):
    a, b = noise_taps(c, uv)
    N1, N2 = T('tf13', *a), T('tf13', *b)
    n = [math.sqrt(abs(N1[k] * N2[k])) for k in range(3)]
    L = 0.30 * rgb[0] + 0.59 * rgb[1] + 0.11 * rgb[2]
    if L <= 0.5:
        ov = [2 * n[k] * rgb[k] for k in range(3)]
    else:
        ov = [1 - 2 * (1 - n[k]) * (1 - rgb[k]) for k in range(3)]
    w = midtone_const * c[113][3] * L * (1 - L) ** 2
    return [rgb[k] + w * (ov[k] - rgb[k]) for k in range(3)], {('tf13',) + a, ('tf13',) + b}


def spot(c, uv, T):
    u, v = uv
    k = c[127][0] + c[127][1] * T('tf5', u, v)[0]
    return [T('tf12', u, v)[i] * k * c[91][0] for i in range(3)]


def m_4FD4(c, uv, T):  # glare + spot + noise, no xfm
    u, v = uv
    sc, L0 = T('tf6', u, v), T('tf7', u, v)
    sp = spot(c, uv, T)
    rgb = [sc[i] + 0.5 * L0[i] * c[6][i] + sp[i] for i in range(3)]
    rgb, nt = noise(c, uv, T, rgb)
    taps = {(s, u, v) for s in ('tf5', 'tf6', 'tf7', 'tf12')} | nt
    return [sat(x) for x in rgb] + [sat(sc[3])], taps


def m_0A9D(c, uv, T):  # DOF + spot + noise
    u, v = uv
    sc, bl = T('tf6', u, v), T('tf8', u, v)
    a = dof_amount(c[24], T('tf9', u, v)[0])
    sp = spot(c, uv, T)
    rgb = [sc[i] + (bl[i] - sc[i]) * a + sp[i] for i in range(3)]
    rgb, nt = noise(c, uv, T, rgb)
    taps = {(s, u, v) for s in ('tf5', 'tf6', 'tf8', 'tf9', 'tf12')} | nt
    return [sat(x) for x in rgb] + [sat(sc[3] + (bl[3] - sc[3]) * a)], taps


def m_D194(c, uv, T):  # DOF + bloom + spot + noise
    u, v = uv
    sc, bl = T('tf6', u, v), T('tf8', u, v)
    a = dof_amount(c[24], T('tf9', u, v)[0])
    Ls = [T(s, u, v) for s in ('tf7', 'tf11', 'tf15')]
    sp = spot(c, uv, T)
    rgb = [sc[i] + (bl[i] - sc[i]) * a for i in range(3)]
    rgb = [1 - (1 - rgb[i]) * (1 - sum(l[i] for l in Ls) * c[6][i]) + sp[i] for i in range(3)]
    rgb, nt = noise(c, uv, T, rgb)
    taps = {(s, u, v) for s in ('tf5', 'tf6', 'tf7', 'tf8', 'tf9', 'tf11', 'tf12', 'tf15')} | nt
    return [sat(x) for x in rgb] + [sat(sc[3] + (bl[3] - sc[3]) * a)], taps


def consts(rng):
    c = consts_composite(rng)
    c[112] = rv(rng, 0, 1)
    c[113] = [rng.uniform(1, 40), rng.uniform(1, 40), rng.uniform(0.5, 2), rng.uniform(-3, 3)]
    return c


CASES = [('4FD492804C1C9070', 'glare+spot+noise', m_4FD4),
         ('0A9D6EAEF05C39F9', 'DOF+spot+noise', m_0A9D),
         ('D1942A59905BAEE8', 'DOF+bloom+spot+noise', m_D194)]


def main(trials=200):
    allok = True
    for h, name, model in CASES:
        rng = random.Random(int(h[:8], 16))
        me = 0
        bad = 0
        for _ in range(trials):
            c = consts(rng)
            uv = (rng.uniform(0, 1), rng.uniform(0, 1))
            tex = Tex(rng)
            got = run_shader(h, c, uv, tex)
            cc = {i: list(v) for i, v in c.items()}
            cc.update(xsim.lits(h))
            exp, taps = model(cc, uv, tex)
            me = max(me, max(abs(g - e) for g, e in zip(got, exp)))
            seen = {(s, round(a, 9), round(b, 9)) for (s, a, b) in tex.log}
            want = {(s, round(a, 9), round(b, 9)) for (s, a, b) in taps}
            if seen != want:
                bad += 1
        ok = me < 1e-4 and bad == 0
        allok &= ok
        print('%s %-22s trials=%d max_err=%.3g tap_mismatch=%d %s'
              % (h, name, trials, me, bad, 'PASS' if ok else 'FAIL'))
    return allok


if __name__ == '__main__':
    sys.exit(0 if main(int(sys.argv[1]) if len(sys.argv) > 1 else 200) else 1)
