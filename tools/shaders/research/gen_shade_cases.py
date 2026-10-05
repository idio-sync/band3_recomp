"""Reference values for tests/shade_model_test.cpp, from the M2 research's Python
models of the game's shaders (fam3.py standard, skin2.py skin, hair3.py hair),
which were checked against the game's microcode. The models are plain maths, so
this needs no shader dump; it prints kCases' entries. The projected light's
cases also give c66, c69 and its two texels (s5, s10), which the others leave
out (zero); the shadow buffer's give those as zero and c107, c108 and lit, how
much of the shadow buffer's taps the pixel passes (the models' one tap: s5's
depth against the shadow coordinate's z/w, 1 or 0). The normal map's give
those (zero where they're not the shadow buffer's) and then the interpolated
tangent (the models' r4, I4) and bitangent (r5, I5), the normal map's texel
(s1), the detail map's (s14), c14 and c106; their occlusion, if any, is in r7,
the tangent frame's in r4 and r5. A hair case with a specular colour then gives
c19, its strands' second colour (the others have none: their strands, which
need the normal map's bitangent, are left out)."""
import sys, os, random, math
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from hyp import norm, sat, dot, refl
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
        prelit=False, rim=False, spec=True, zero_c2=False, proj=False, gobo=False,
        shadow=None, nmap=False, detail=False, across=False):
    c, P, eye, N, vc, t = make(seed, npt)
    # The normal map: drawn after make()'s, so the other cases keep their
    # numbers. A tangent frame of any length and direction, as the
    # interpolators give it, the maps' texels away from flat, c14 (1 -
    # de_normal) below 1 so the softening shows, c106 the detail's share and
    # its uv scale (unread here: the texel is given).
    U = Bt = None
    if nmap:
        U = [round(random.uniform(-1, 1), 3) for _ in range(3)]
        Bt = [round(random.uniform(-1, 1), 3) for _ in range(3)]
        t['tf1'] = r4(0.1, 0.9)
        t['tf14'] = r4(0.2, 0.8)
        c[14] = [round(random.uniform(0.3, 1.0), 3), 0.0, 0.0, 0.0]
        c[106] = [round(random.uniform(0.2, 0.8), 3), round(random.uniform(1, 8), 2), 0.0, 0.0]
    if proj:
        # drawn after make()'s, so the other cases keep their numbers
        # toward it from about where the surface faces, so that it counts
        d = norm([N[k] + random.uniform(-0.6, 0.6) for k in range(3)])
        c[66] = [round(x, 3) for x in d] + [0.0]
        c[69] = r4(0.3, 1.0); c[69][3] = 1.0
        # s5's alpha: much of it for the multiply, little for the gobo it masks
        t['tf5'] = r4(0.0, 0.6) if gobo else r4(0.4, 1.0)
        t['tf10'] = r4(0.0, 1.0)
    # The shadow buffer (shadow: lit, 0 or 1): its coordinate in r6 at z/w
    # 0.6, s5's depth in front of it (0.3, shadowed) or behind (0.9, lit).
    # c108 the light's forward, from about behind the surface (sat(N.-c108)
    # counts), c107 1 - the shadow's colour.
    if shadow is not None:
        d = norm([-N[k] + random.uniform(-0.6, 0.6) for k in range(3)])
        c[108] = [round(x, 3) for x in d] + [1.0]
        c[107] = r4(0.3, 1.0); c[107][3] = 0.0
        t['tf5'] = [0.9 if shadow else 0.3, 0.0, 0.0, 1.0]
    if zero_c2:
        c[2][0] = c[2][1] = c[2][2] = 0.0
        c[19] = [0, 0, 0, 1]
    elif family == 'hair':
        # the strands' second colour, drawn last so the rest keep their numbers
        c[19] = r4(0.2, 1.0)
    if across:
        # The strands across the eye's reflection and point light 0 along it,
        # where their highlight is: the normal map flat along the bitangent
        # (x = 0, the detail map's too), so the normal doesn't depend on it,
        # then a power of 8 to 30 and the bitangent, at about unit length, at
        # the angle to R (hair3.py's N and R) that puts the broad colour's
        # weight s^2p at 0.3 to 0.8, so both colours show.
        t['tf1'][0] = 0.5
        t['tf14'][0] = 0.5
        ny = t['tf1'][1] * 2 - 1
        nz = sat(1 - ny * ny)
        if detail:
            dy = t['tf14'][1] * 2 - 1
            ny += c[106][0] * dy
            nz += c[106][0] * sat(1 - dy * dy)
        Nm = norm([nz * N[k] + c[14][0] * ny * U[k] for k in range(3)])
        R = refl(Nm, norm([eye[k] - P[k] for k in range(3)]))
        a = [random.uniform(-1, 1) for _ in range(3)]
        side = norm([R[1] * a[2] - R[2] * a[1], R[2] * a[0] - R[0] * a[2], R[0] * a[1] - R[1] * a[0]])
        c[2][3] = round(random.uniform(8, 30), 2)
        t['tf2'][3] = round(random.uniform(0.4, 1.0), 3)
        p = max(t['tf2'][3] * c[2][3], 0.5)
        rt = math.sqrt(1 - random.uniform(0.3, 0.8) ** (1 / (2 * p)))
        size = random.uniform(0.95, 1.05)
        Bt = [round(size * (math.sqrt(1 - rt * rt) * side[k] + rt * R[k]), 3) for k in range(3)]
        d = random.uniform(80, 200)
        c[64][:3] = [round(P[k] + d * R[k] + random.uniform(-5, 5), 2) for k in range(3)]
    if not spec:
        c[2] = [0, 0, 0, c[2][3]]
    if 'Box' not in flags:
        for k in range(6):
            c[80 + k] = [0, 0, 0, 0]
    r = {1: P + [1], 2: [eye[k] - P[k] for k in range(3)] + [0], 3: N + [0]}
    # the occlusion's register: r7 where r4 and r5 are the tangent frame
    aoreg = 7 if nmap else 4
    if prelit:
        r[4] = vc
    elif ao is not None:
        r[aoreg] = ao_of(vc, ao)
    else:
        r[aoreg] = [1, 1, 1, 1]
    if aoreg != 4:
        r[4] = [1, 1, 1, 1]
    r[5] = [0, 0, 0, 0]
    r[6] = [0.2, 0.3, 0.6, 1.0]
    if nmap:
        r[4] = U + [0]
        r[5] = Bt + [0]
    shreg = 6 if shadow is not None else 0
    if not glow:
        c[5][0] = 0.0
    if family == 'standard':
        o = dict(nmap=nmap, detail=detail, rim=rim, rimmap=False, hair=False, proj=proj,
                 gobo=gobo, npt=npt, spec=spec, glow=glow, intens=intens, tex=True,
                 specmap=specmap, col='vc' if prelit else ('ao' if ao is not None else 'none'),
                 aoreg=aoreg, shadow=shreg, shall=True, env=None, rimnz=False)
        rgb, _ = fam3.model(c, r, t, o)
    elif family == 'skin':
        o = dict(nmap=nmap, detail=detail, rim=rim, npt=npt, specmap=specmap, tex=True,
                 ao=ao is not None, aoreg=aoreg, shadow=shreg)
        rgb = skin2.skin(c, r, t, o)
        if glow:
            rgb = [rgb[k] + t['tf3'][k] * c[5][0] for k in range(3)]
    else:
        o = dict(detail=detail, npt=npt, ao=ao is not None, aoreg=aoreg, shadow=shreg)
        if not specmap:
            t['tf2'] = [1, 1, 1, 1]
        rgb = hair3.hair(c, r, t, o)
    tex = t['tf0']
    a_tex = tex[3] * (c[5][1] if intens else 1)
    alpha = a_tex * c[1][3] * (vc[3] if prelit else c[0][3])
    cases.append(dict(name=name, flags=flags, npt=npt, c=c, P=P, eye=eye, N=N, vc=vc, t=t,
                      ao=ao or 0.0, rgb=rgb, alpha=alpha, proj=proj, shadow=shadow, nmap=nmap,
                      U=U, B=Bt, c19=family == 'hair' and not zero_c2))

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
add('standard: projected light, multiply, AO, rim', 'standard',
    ['Lit', 'Box', 'Specular', 'Rim', 'AO', 'ProjMultiply', 'Textured'], 9, 1, ao=1.2, rim=True,
    proj=True)
add('standard: projected light, multiply, prelit, two points', 'standard',
    ['Lit', 'Box', 'Specular', 'Prelit', 'ProjMultiply', 'Textured'], 10, 2, prelit=True,
    proj=True)
add('standard: projected light, gobo, AO, two points', 'standard',
    ['Lit', 'Box', 'Specular', 'AO', 'ProjGobo', 'Textured'], 11, 2, ao=0.9, proj=True, gobo=True)
add('standard: in shadow, two points, rim, AO', 'standard',
    ['Lit', 'Box', 'Specular', 'Rim', 'AO', 'Shadow', 'Textured'], 12, 2, ao=0.8, rim=True,
    shadow=0)
add('standard: shadow buffer, lit, prelit', 'standard',
    ['Lit', 'Box', 'Specular', 'Prelit', 'Shadow', 'Textured'], 13, 1, prelit=True, shadow=1)
add('skin: in shadow, rim, AO, two points', 'skin',
    ['Lit', 'Box', 'Specular', 'Rim', 'AO', 'Skin', 'Shadow', 'Textured'], 14, 2, ao=1.0,
    rim=True, shadow=0)
add('hair: in shadow, two points', 'hair',
    ['Lit', 'Box', 'Specular', 'SpecMap', 'Hair', 'Shadow', 'Textured'], 15, 2, specmap=True,
    zero_c2=True, shadow=0)
add('standard: normal map, two points, rim', 'standard',
    ['Lit', 'Box', 'Specular', 'Rim', 'NormalMap', 'Textured'], 16, 2, rim=True, nmap=True)
add('standard: normal and detail maps, AO, specular map', 'standard',
    ['Lit', 'Box', 'Specular', 'SpecMap', 'AO', 'NormalMap', 'DetailMap', 'Textured'], 17, 1,
    ao=1.1, specmap=True, nmap=True, detail=True)
add('standard: normal map, in shadow, two points', 'standard',
    ['Lit', 'Box', 'Specular', 'Shadow', 'NormalMap', 'Textured'], 18, 2, shadow=0, nmap=True)
add('skin: normal and detail maps, rim, AO, two points', 'skin',
    ['Lit', 'Box', 'Specular', 'SpecMap', 'Rim', 'AO', 'Skin', 'NormalMap', 'DetailMap',
     'Textured'], 19, 2, ao=1.0, specmap=True, rim=True, nmap=True, detail=True)
add('skin: normal and detail maps, in shadow, rim', 'skin',
    ['Lit', 'Box', 'Specular', 'Rim', 'Skin', 'Shadow', 'NormalMap', 'DetailMap', 'Textured'],
    20, 2, rim=True, shadow=0, nmap=True, detail=True)
add('hair: normal and detail maps, two points', 'hair',
    ['Lit', 'Box', 'Specular', 'SpecMap', 'Hair', 'NormalMap', 'DetailMap', 'Textured'], 21, 2,
    specmap=True, zero_c2=True, nmap=True, detail=True)
add('hair: strands, normal and detail maps, two points', 'hair',
    ['Lit', 'Box', 'Specular', 'SpecMap', 'Hair', 'NormalMap', 'DetailMap', 'Textured'], 22, 2,
    specmap=True, nmap=True, detail=True)
add('hair: strands across the reflection, normal and detail maps, AO, in shadow', 'hair',
    ['Lit', 'Specular', 'SpecMap', 'AO', 'Hair', 'Shadow', 'NormalMap', 'DetailMap', 'Textured'],
    23, 1, ao=1.0, specmap=True, shadow=0, nmap=True, detail=True, across=True)
add('hair: strands across the reflection, normal map, two points', 'hair',
    ['Lit', 'Box', 'Specular', 'SpecMap', 'Hair', 'NormalMap', 'Textured'], 24, 2,
    specmap=True, nmap=True, across=True)
add('hair: strands across the reflection, normal and detail maps, AO, shadow lit', 'hair',
    ['Lit', 'Specular', 'SpecMap', 'AO', 'Hair', 'Shadow', 'NormalMap', 'DetailMap', 'Textured'],
    25, 1, ao=0.7, specmap=True, shadow=1, nmap=True, detail=True, across=True)

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
    zero = [0, 0, 0, 0]
    end = ',' if cs['nmap'] else '},'
    if cs['proj']:
        out.append('     %s, %s,' % (f4(cs['rgb']), lit(cs['alpha'])))
        out.append('     %s, %s, %s, %s%s' % (f4(c[66]), f4(c[69]), f4(cs['t']['tf5']),
                                             f4(cs['t']['tf10']), end))
    elif cs['shadow'] is not None:
        out.append('     %s, %s,' % (f4(cs['rgb']), lit(cs['alpha'])))
        out.append('     %s, %s, %s, %s,' % (f4(zero), f4(zero), f4(zero), f4(zero)))
        out.append('     %s, %s, %s%s' % (f4(c[107]), f4(c[108]), lit(float(cs['shadow'])), end))
    elif cs['nmap']:
        out.append('     %s, %s,' % (f4(cs['rgb']), lit(cs['alpha'])))
        out.append('     %s, %s, %s, %s,' % (f4(zero), f4(zero), f4(zero), f4(zero)))
        out.append('     %s, %s, %s,' % (f4(zero), f4(zero), lit(1.0)))
    else:
        out.append('     %s, %s},' % (f4(cs['rgb']), lit(cs['alpha'])))
    if cs['nmap']:
        out.append('     %s, %s, %s, %s,' % (f4(cs['U']), f4(cs['B']), f4(cs['t']['tf1']),
                                             f4(cs['t']['tf14'])))
        if cs['c19']:
            out.append('     %s, %s, %s},' % (f4(c[14]), f4(c[106]), f4(c[19])))
        else:
            out.append('     %s, %s},' % (f4(c[14]), f4(c[106])))
print('\n'.join(out))
