"""Reference values for tests/shade_model_test.cpp, from the M2 research's Python
models of the game's shaders (fam3.py standard, skin2.py skin, hair3.py hair),
which were checked against the game's microcode. The models are plain maths, so
this needs no shader dump; it prints kCases' entries."""
import sys, os, random, math
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from hyp import norm, sat
import fam3, skin2, hair3

def r4(lo, hi):
    return [round(random.uniform(lo, hi), 3) for _ in range(4)]

def make(seed, npt):
    random.seed(seed)
    c = {}
    c[0] = r4(0.2, 1.0)
    c[1] = r4(0.0, 0.4)
    c[2] = r4(0.2, 1.0); c[2][3] = round(random.uniform(2, 30), 2)
    c[5] = [round(random.uniform(0.5, 2), 3), 2.0, 0.0, 0.0]
    c[7] = [0.03, 0.059, 0.011, 10.0]
    c[14] = [1.0, 1.0, 1.0, 1.0]
    c[63] = r4(0.2, 1.0); c[63][3] = round(random.uniform(1, 4), 2)
    for i in range(2):
        c[64 + i] = [round(random.uniform(-200, 200), 2), round(random.uniform(-200, 200), 2),
                     round(random.uniform(50, 300), 2), round(-1.0 / random.uniform(200, 600), 6)]
        c[67 + i] = r4(0.3, 2.0); c[67 + i][3] = round(random.uniform(1.0, 2.0), 3)
        if i >= npt:
            c[64 + i] = [0, 0, 0, 0]; c[67 + i] = [0, 0, 0, 1]
    for k in range(6):
        c[80 + k] = r4(0.0, 0.6)
    P = [round(random.uniform(-50, 50), 2) for _ in range(3)]
    eye = [round(random.uniform(-300, 300), 2), round(random.uniform(-600, -300), 2),
           round(random.uniform(100, 300), 2)]
    N = [round(random.uniform(-1, 1), 3) for _ in range(3)]
    vc = r4(0.0, 1.0)
    t = {'tf0': r4(0.1, 1.0), 'tf2': r4(0.1, 1.0), 'tf3': r4(0.0, 1.0), 'tf1': [0.5, 0.5, 0.5, 1.0]}
    return c, P, eye, N, vc, t

def ao_of(vc, s):
    a = sat(1 + s * (1.128379 * vc[0] - 1))
    d = sat(1 + s * (1.504505 * vc[0] - 1))
    return [d, a, a, a]

cases = []
def add(name, family, flags, seed, npt, ao=None, specmap=False, glow=False, intens=False,
        prelit=False, rim=False, spec=True, zero_c2=False):
    c, P, eye, N, vc, t = make(seed, npt)
    if zero_c2:
        c[2][0] = c[2][1] = c[2][2] = 0.0
        c[19] = [0, 0, 0, 1]
    if not spec:
        c[2] = [0, 0, 0, c[2][3]]
    if 'Box' not in flags:
        for k in range(6):
            c[80 + k] = [0, 0, 0, 0]
    r = {1: P + [1], 2: [eye[k] - P[k] for k in range(3)] + [0], 3: N + [0]}
    if prelit:
        r[4] = vc
    elif ao is not None:
        r[4] = ao_of(vc, ao)
    else:
        r[4] = [1, 1, 1, 1]
    r[5] = [0, 0, 0, 0]
    if not glow:
        c[5][0] = 0.0
    if family == 'standard':
        o = dict(nmap=False, detail=False, rim=rim, rimmap=False, hair=False, proj=False,
                 npt=npt, spec=spec, glow=glow, intens=intens, tex=True, specmap=specmap,
                 col='vc' if prelit else ('ao' if ao is not None else 'none'), aoreg=4,
                 shadow=0, shall=False, env=None, rimnz=False)
        rgb, _ = fam3.model(c, r, t, o)
    elif family == 'skin':
        o = dict(nmap=False, detail=False, rim=rim, npt=npt, specmap=specmap, tex=True,
                 ao=ao is not None, aoreg=4, shadow=0)
        rgb = skin2.skin(c, r, t, o)
        if glow:
            rgb = [rgb[k] + t['tf3'][k] * c[5][0] for k in range(3)]
    else:
        o = dict(detail=False, npt=npt, ao=ao is not None, aoreg=4, shadow=0)
        if not specmap:
            t['tf2'] = [1, 1, 1, 1]
        rgb = hair3.hair(c, r, t, o)
    tex = t['tf0']
    a_tex = tex[3] * (c[5][1] if intens else 1)
    alpha = a_tex * c[1][3] * (vc[3] if prelit else c[0][3])
    cases.append(dict(name=name, flags=flags, npt=npt, c=c, P=P, eye=eye, N=N, vc=vc, t=t,
                      ao=ao or 0.0, rgb=rgb, alpha=alpha))

add('standard: two points, box, specular', 'standard', ['Lit', 'Box', 'Specular', 'Textured'], 1, 2)
add('standard: AO, one point, specular map, glow, intensify', 'standard',
    ['Lit', 'Box', 'Specular', 'SpecMap', 'AO', 'Glow', 'Intensify', 'Textured'], 2, 1,
    ao=1.5, specmap=True, glow=True, intens=True)
add('standard: prelit, two points', 'standard', ['Lit', 'Box', 'Specular', 'Prelit', 'Textured'],
    3, 2, prelit=True)
add('standard: rim, AO', 'standard', ['Lit', 'Box', 'Specular', 'Rim', 'AO', 'Textured'], 4, 2,
    ao=0.8, rim=True)
add('standard: no box, no specular', 'standard', ['Lit', 'Textured'], 5, 1, spec=False)
add('skin: rim, AO, specular map', 'skin',
    ['Lit', 'Box', 'Specular', 'SpecMap', 'Rim', 'AO', 'Skin', 'Textured'], 6, 2, ao=1.0,
    specmap=True, rim=True)
add('skin: plain', 'skin', ['Lit', 'Box', 'Specular', 'Skin', 'Textured'], 7, 1)
add('hair: no specular colour (its strands need the tangent)', 'hair',
    ['Lit', 'Box', 'Specular', 'SpecMap', 'Hair', 'Textured'], 8, 2, specmap=True, zero_c2=True)

def lit(x):
    s = '%.9g' % x
    if '.' not in s and 'e' not in s:
        s += '.0'
    return s + 'f'

def f4(v):
    return '{%s}' % ', '.join(lit(x) for x in v)

out = []
for cs in cases:
    c = cs['c']
    out.append('    {"%s",\n     %s, %d,' % (cs['name'], ' | '.join('kShade' + f for f in cs['flags']), cs['npt']))
    regs = [0, 1, 2, 5, 7, 63, 64, 65, 67, 68, 80, 81, 82, 83, 84, 85]
    out.append('     {%s},' % ', '.join(f4(c[k]) for k in regs))
    out.append('     %s, %s, %s, %s, %s,' % (f4(cs['P']), f4(cs['eye']), f4(cs['N']), f4(cs['vc']), lit(cs['ao'])))
    out.append('     %s, %s, %s,' % (f4(cs['t']['tf0']), f4(cs['t']['tf2']), f4(cs['t']['tf3'])))
    out.append('     %s, %s},' % (f4(cs['rgb']), lit(cs['alpha'])))
print('\n'.join(out))
