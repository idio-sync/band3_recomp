"""Check RB3's camera motion blur (velocity blur) against Python models.

RndVelocityBuffer::Draw (retail 0x82B855F0) draws kVelocityCameraShader, PS
8CDB397D65E92379, over a w/2 x h/2 velocity texture with DxRnd::DrawRectDepth:
r0.xy the pixel's uv, r1 the frustum's near point and r2 its corner ray (the
DrawRectDepth VS CB746EC8CA23A7D1 passes them through), s9 the pre-pass depth
(1 - z), c89 the camera's depth range values (near, far, z scale, z bias) and
c134..c137 the previous frame's view-projection. It rebuilds the pixel's view
depth and world position, projects that with the previous matrix and writes
d = clamp(prev uv - uv, +-0.02) as d * 25 + 0.5 (xy) and |d| * 1.7677668 (z),
w 0.

The composites with c122 (NgPostProc::DoVelocity sets TheShaderMgr + 0x39 and
c122 = min(2, 41.67 / (ms + 1))) read s10, the velocity texture, at uv; where
its z is 0.003 or more they take the scene (s6) as 11 taps along the motion,
step 0.0046 * (v.xy - 0.5) * c122.x: the centre at uv weighted g0 and ten at
uv + k * step, k -5..4, weighted g5 g4 g3 g2 g1 g1 g2 g3 g4 g5 (a Gaussian of
sigma 2 steps), all times 1.0054859; and that blurred scene replaces the
scene everywhere after (the DOF lerp, bloom, alpha...).

RndVelocityBuffer::DrawMesh then draws the meshes of the motion blur's list
(the characters) over that with their own motion: VS 21A0C657F6C70854
(skinned) or F922317D5AAC4AE6 places each vertex by this frame's palette
(c9..) and the last frame's (c129..), through this frame's view-projection
(c0..c3) and the last's (c4..c7); PS 39DE58D45328C089 writes prev uv - uv as
the camera pass encodes it, with alpha 1 where the mesh's w is at most the
scene's view depth (s9 through c8) + 1, else 0.

Checks, each with random constants, texture contents and uv:
  - 8CDB397D against the velocity model;
  - 140762E9AE284A66 (velocity and the colour matrix only) against a full
    model of it;
  - every dumped composite that reads c122: its fetches of s6 are uv and,
    within the mask, the ten taps; and its output equals its own output with
    the mask off (s10.z = 0) and the blurred scene in place of s6(uv), so the
    blur substitutes for the scene and nothing else changes;
  - the object pass's two vertex shaders (oPos, o0 and o1) and its pixel
    shader (its texel and its s9 tap) against their models.

Usage: python check_velocity.py [trials]   (default 100)
       python check_velocity.py --neg        wrong models, which must FAIL
"""
import glob
import math
import os
import random
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), '..'))
import xsim
# check_post reads its trial count from the command line as it loads
_argv, sys.argv = sys.argv, sys.argv[:1]
from check_post import Tex, rv, sat
sys.argv = _argv

VELOCITY = '8CDB397D65E92379'
VELOCITY_XFM = '140762E9AE284A66'

# the composite's Gaussian: the centre's weight, then k = -5..4's
G0 = 0.1994711458683014
GK = [0.00876415055245161, 0.02699548378586769, 0.06475879997015, 0.12098535895347595,
      0.1760326623916626, 0.1760326623916626, 0.12098535895347595, 0.06475879997015,
      0.02699548378586769, 0.00876415055245161]
NORM = 1.0054858922958374
STEP = 0.0046
# a negative control's: the k = 0 tap left out (the centre alone)
SKIP_ZERO = False


def velocity_model(c, uv, near, corner, depth):
    """8CDB397D: the velocity texel (RGBA) at uv"""
    n, f, zs, zb = c[89]
    z = (1.0 - depth) * zs - zb
    w = n * f / (f - z * (f - n))
    p = [near[i] + w / f * corner[i] for i in range(3)] + [1.0]
    px = sum(c[134][i] * p[i] for i in range(4))
    py = sum(c[135][i] * p[i] for i in range(4))
    pw = sum(c[137][i] * p[i] for i in range(4))
    d = [0.5 + 0.5 * px / pw - uv[0], 0.5 - 0.5 * py / pw - uv[1]]
    d = [min(max(x, -0.02), 0.02) for x in d]
    return [d[0] * 25 + 0.5, d[1] * 25 + 0.5, math.sqrt(d[0] ** 2 + d[1] ** 2) * 1.7677668333053589,
            0.0]


def blur_taps(c, uv, vel):
    """the scene's taps within the mask (k = -5..4), or none outside it"""
    if not vel[2] >= 0.003:
        return []
    s = [STEP * (vel[i] - 0.5) * c[122][0] for i in range(2)]
    return [(uv[0] + k * s[0], uv[1] + k * s[1]) for k in range(-5, 5) if k or not SKIP_ZERO]


def blurred_scene(c, uv, T):
    vel = T('tf10', *uv)
    scene = T('tf6', *uv)
    taps = blur_taps(c, uv, vel)
    if not taps:
        return scene, []
    acc = [scene[i] * G0 for i in range(4)]
    for (a, b), g in zip(taps, GK if not SKIP_ZERO else GK[:5] + GK[6:]):
        t = T('tf6', a, b)
        acc = [acc[i] + t[i] * g for i in range(4)]
    return [x * NORM for x in acc], taps


def m_velocity_xfm(c, uv, T):
    """140762E9: the velocity blur, then the colour matrix; alpha the blurred scene's"""
    sc, taps = blurred_scene(c, uv, T)
    out = [sat(sum(c[92 + ch][k] * sc[k] for k in range(3)) + c[92 + ch][3]) for ch in range(3)]
    return out + [sat(sc[3])], taps


class FixedTex(Tex):
    """A texture smooth in its coordinate (a sum of sines per sampler, so the
    shader's taps, which it steps to by adding, read what the model's do to
    float error), with some (sampler, coordinate)s pinned"""
    def __init__(self, rng, pinned):
        super().__init__(rng)
        self.pinned = dict(pinned)
        self.waves = {}

    def __call__(self, unit, u, v):
        for (pu, a, b), val in self.pinned.items():
            if pu == unit and abs(a - u) < 1e-7 and abs(b - v) < 1e-7:
                return val
        if unit not in self.waves:
            self.waves[unit] = [[self.rng.uniform(5, 60), self.rng.uniform(5, 60),
                                 self.rng.uniform(0, 6.3)] for _ in range(4)]
        return [0.5 + 0.5 * math.sin(fu * u + fv * v + ph) for fu, fv, ph in self.waves[unit]]

    def fetch_cb(self, op, unit, coord, line):
        self.log.append((unit, coord[0], coord[1]))
        return list(self(unit, coord[0], coord[1]))


def same_taps(seen, want, tol=1e-7):
    """the same coordinates, to float error, as many times each"""
    seen, want = sorted(seen), sorted(want)
    return len(seen) == len(want) and all(
        abs(a[0] - b[0]) < tol and abs(a[1] - b[1]) < tol for a, b in zip(seen, want))


def run(h, c, uv, tex, regs=None):
    cc = {i: [0.0] * 4 for i in range(256)}
    cc.update(c)
    cc.update(xsim.lits(h))
    r = {0: [uv[0], uv[1], 0.0, 0.0]}
    r.update(regs or {})
    st = xsim.State(cc, r, tex.fetch_cb)
    xsim.run(xsim.load(h), st)
    return st.o.get('oC0', [math.nan] * 4)


def key(unit, u, v):
    return (unit, u, v)


def check_velocity(trials):
    rng = random.Random(0x8CDB397D)
    me = 0.0
    bad = 0
    for t in range(trials):
        c = {i: rv(rng, -1, 1) for i in range(256)}
        n, f = rng.uniform(0.5, 20), rng.uniform(500, 20000)
        zr = sorted([rng.uniform(0, 0.2), rng.uniform(0.8, 1)])
        c[89] = [n, f, 1.0 / (zr[1] - zr[0]), zr[0] / (zr[1] - zr[0])]
        # a previous matrix near a real one (a moving camera's): clip w well
        # away from 0, so prev / w is finite
        for r in range(4):
            c[134 + r] = rv(rng, -1, 1)
        c[137] = [rng.uniform(-0.01, 0.01), rng.uniform(-0.01, 0.01), rng.uniform(-0.01, 0.01),
                  rng.uniform(5, 50)]
        uv = (rng.uniform(0, 1), rng.uniform(0, 1))
        near = [rng.uniform(-10, 10) for _ in range(3)] + [0.0]
        corner = [rng.uniform(-f, f) for _ in range(3)] + [0.0]
        # half the trials a small motion (within the clamp), half anything
        if t % 2 == 0:
            c[134] = [x * 0.001 for x in c[134]]
            c[135] = [x * 0.001 for x in c[135]]
        depth = rng.uniform(0, 1)
        tex = FixedTex(rng, {key('tf9', *uv): [depth, 0, 0, 0]})
        got = run(VELOCITY, c, uv, tex, {1: near, 2: corner})
        cc = dict(c)
        exp = velocity_model(cc, uv, near, corner, depth)
        err = max(abs(g - e) for g, e in zip(got, exp))
        me = max(me, err)
        if [s for (s, a, b) in tex.log] != ['tf9'] or not same_taps([l[1:] for l in tex.log], [uv]):
            bad += 1
    ok = me < 1e-4 and bad == 0
    print('%s %-34s trials=%d max_err=%.3g tap_mismatch=%d %s'
          % (VELOCITY, 'velocity camera pass', trials, me, bad, 'PASS' if ok else 'FAIL'))
    return ok


def composite_consts(rng):
    c = {i: rv(rng, -1, 1) for i in range(256)}
    c[6] = rv(rng, 0, 2)
    s, b = rng.uniform(0, 1), rng.uniform(0, 1)
    if abs(s - b) < 1e-3:
        b = s + 0.01
    mx, mn = rng.uniform(0, 1), rng.uniform(0, 1)
    c[24] = [1 / (s - b), -s / (s - b), min(mx, mn), mx]
    c[112] = rv(rng, 0, 1)
    c[113] = [rng.uniform(1, 40), rng.uniform(1, 40), rng.uniform(0.5, 2), rng.uniform(-3, 3)]
    c[122] = [rng.uniform(0, 2)] * 4
    c[125] = [rng.uniform(0, 1), rng.uniform(0, 1), 1.0 / 3.0, 0.0]
    return c


def vel_texel(rng, on):
    """a velocity texel: within the mask (|d| from 0.0017 up) or under it"""
    if on:
        d = [rng.uniform(-0.02, 0.02), rng.uniform(-0.02, 0.02)]
        while math.hypot(*d) * 1.7677668 < 0.0031:
            d = [rng.uniform(-0.02, 0.02), rng.uniform(-0.02, 0.02)]
    else:
        d = [rng.uniform(-0.0012, 0.0012), rng.uniform(-0.0012, 0.0012)]
    return [d[0] * 25 + 0.5, d[1] * 25 + 0.5, math.hypot(*d) * 1.7677668, 0.0]


def check_xfm(trials):
    rng = random.Random(0x140762E9)
    me = 0.0
    bad = 0
    for t in range(trials):
        c = composite_consts(rng)
        uv = (rng.uniform(0, 1), rng.uniform(0, 1))
        tex = FixedTex(rng, {key('tf10', *uv): vel_texel(rng, t % 4 != 0)})
        got = run(VELOCITY_XFM, c, uv, tex)
        cc = dict(c)
        cc.update(xsim.lits(VELOCITY_XFM))
        exp, taps = m_velocity_xfm(cc, uv, tex)
        me = max(me, max(abs(g - e) for g, e in zip(got, exp)))
        if not (same_taps([l[1:] for l in tex.log if l[0] == 'tf6'], [uv] + taps) and
                same_taps([l[1:] for l in tex.log if l[0] == 'tf10'], [uv]) and
                {l[0] for l in tex.log} <= {'tf6', 'tf10'}):
            bad += 1
    ok = me < 1e-4 and bad == 0
    print('%s %-34s trials=%d max_err=%.3g tap_mismatch=%d %s'
          % (VELOCITY_XFM, 'composite velocity+xfm (model)', trials, me, bad,
             'PASS' if ok else 'FAIL'))
    return ok


def check_substitution(h, trials):
    """the blurred scene stands in for s6(uv), and nothing else changes"""
    rng = random.Random(int(h[:8], 16))
    me = 0.0
    bad = 0
    on_count = 0
    for t in range(trials):
        c = composite_consts(rng)
        uv = (rng.uniform(0.05, 0.95), rng.uniform(0.05, 0.95))
        on = t % 4 != 0
        tex = FixedTex(rng, {key('tf10', *uv): vel_texel(rng, on)})
        got = run(h, c, uv, tex)
        cc = dict(c)
        cc.update(xsim.lits(h))
        blurred, taps = blurred_scene(cc, uv, tex)
        on_count += bool(taps)
        # the same shader, the same textures, the mask off and the blurred
        # scene at uv
        ref = FixedTex(rng, {key('tf10', *uv): [0.5, 0.5, 0.0, 0.0],
                             key('tf6', *uv): blurred})
        ref.waves = tex.waves
        want = run(h, c, uv, ref)
        me = max(me, max(abs(g - e) for g, e in zip(got, want)))
        if not same_taps([l[1:] for l in tex.log if l[0] == 'tf6'], [uv] + taps):
            bad += 1
    ok = me < 1e-4 and bad == 0 and on_count > 0
    print('%s %-34s trials=%d max_err=%.3g tap_mismatch=%d %s'
          % (h, 'composite: blur stands in for s6', trials, me, bad, 'PASS' if ok else 'FAIL'))
    return ok


# ---------------------------------------------------------------- objects
# RndVelocityBuffer::DrawMesh draws each mesh of NgPostProc's
# mMotionBlurDrawList (the characters) over the camera pass, in draw mode 5,
# with kVelocityObjectShader: VS 21A0C657F6C70854 (skinned) or F922317D5AAC4AE6
# (not), PS 39DE58D45328C089. DrawMesh runs after Draw's AdvanceFrame has
# flipped the index, so VS c0..c3 are this frame's view-projection and c4..c7
# the previous one's, c9.. this frame's bone palette and c129.. the last
# frame's (3 rows each, the transposed 3x4 the colour pass's VS reads too),
# PS c8 the camera's depth range values (c89's).
OBJECT_VS = '21A0C657F6C70854'
OBJECT_VS_RIGID = 'F922317D5AAC4AE6'
OBJECT_PS = '39DE58D45328C089'


def dot4(a, b):
    return sum(a[i] * b[i] for i in range(4))


def object_vs_model(c, pos, weights, bones):
    """o0 (and oPos) this frame's clip position, o1 the last frame's: the
    position skinned by each palette (c9.. and c129..: rows dotted with it),
    weights x, y, z and 1 - their sum, by bones x, y, z, w; without bones
    (`weights` None) the first palette's only matrix"""
    p = list(pos) + [1.0]
    def skin(first):
        if weights is None:
            ws, ix = [1.0], [0]
        else:
            ws = list(weights) + [1.0 - sum(weights)]
            ix = list(bones)
        out = [0.0, 0.0, 0.0]
        for w, i in zip(ws, ix):
            for r in range(3):
                out[r] += w * dot4(c[first + 3 * i + r], p)
        return out + [1.0]
    cur, prev = skin(9), skin(129)
    return ([dot4(c[r], cur) for r in range(4)], [dot4(c[4 + r], prev) for r in range(4)])


def check_object_vs(h, skinned, trials):
    rng = random.Random(int(h[:8], 16))
    me = 0.0
    for _ in range(trials):
        c = {i: rv(rng, -1, 1) for i in range(256)}
        pos = [rng.uniform(-50, 50) for _ in range(3)]
        weights = [rng.uniform(0, 0.4) for _ in range(3)] if skinned else None
        bones = [rng.randrange(0, 40) for _ in range(4)] if skinned else None

        def fetch(op, unit, coord, line):
            if 'Offset=7' in line:  # FMT_2_10_10_10: the weights
                return list(weights) + [0.0]
            if 'Offset=8' in line:  # FMT_8_8_8_8: the bones
                return [float(b) for b in bones]
            return pos + [1.0]
        cc = dict(c)
        cc.update(xsim.lits(h))
        st = xsim.State(cc, {0: [0.0, 0.0, 0.0, 0.0]}, fetch)
        xsim.run(xsim.load(h, 'vert'), st)
        cur, prev = object_vs_model(c, pos, weights, bones)
        got = st.o.get('oPos'), st.o.get('o0'), st.o.get('o1')
        if not all(got):
            me = math.inf
            continue
        for g, e in ((got[0], cur), (got[1], cur), (got[2], prev)):
            scale = max(1.0, max(abs(x) for x in e))
            me = max(me, max(abs(a - b) for a, b in zip(g, e)) / scale)
    ok = me < 1e-6
    print('%s %-34s trials=%d max_rel_err=%.3g %s'
          % (h, 'object VS %s' % ('skinned' if skinned else 'rigid'), trials, me,
             'PASS' if ok else 'FAIL'))
    return ok


def object_ps_model(c, cur, prev, depth):
    """39DE58D4: the velocity texel, rgba: prev uv - uv as the camera pass
    keeps it, and alpha 1 where the mesh is in front of the scene's depth
    there (its clip w at most the scene's view depth + 1), else 0 (SrcAlpha
    keeps the camera's texel then); s9 read at this frame's uv"""
    cu, cv = 0.5 + 0.5 * cur[0] / cur[3], 0.5 - 0.5 * cur[1] / cur[3]
    pu, pv = 0.5 + 0.5 * prev[0] / prev[3], 0.5 - 0.5 * prev[1] / prev[3]
    n, f, zs, zb = c[8]
    z = (1.0 - depth) * zs - zb
    w = n * f / (f - z * (f - n))
    d = [min(max(pu - cu, -0.02), 0.02), min(max(pv - cv, -0.02), 0.02)]
    a = 0.0 if cur[3] > w + 1.0 else 1.0
    return [d[0] * 25 + 0.5, d[1] * 25 + 0.5, math.hypot(*d) * 1.7677668333053589, a], (cu, cv)


def check_object_ps(trials):
    rng = random.Random(0x39DE58D4)
    me = 0.0
    bad = 0
    for t in range(trials):
        c = {i: rv(rng, -1, 1) for i in range(256)}
        n, f = rng.uniform(1, 20), rng.uniform(500, 20000)
        zr = sorted([rng.uniform(0, 0.2), rng.uniform(0.8, 1)])
        c[8] = [n, f, 1.0 / (zr[1] - zr[0]), zr[0] / (zr[1] - zr[0])]
        cw = rng.uniform(20, 2000)
        cur = [rng.uniform(-cw, cw), rng.uniform(-cw, cw), rng.uniform(0, cw), cw]
        pw = cw * rng.uniform(0.9, 1.1)
        k = 0.01 if t % 2 else 1.0  # small motion half the time
        prev = [cur[0] * pw / cw + rng.uniform(-k, k) * pw, cur[1] * pw / cw + rng.uniform(-k, k) * pw,
                rng.uniform(0, pw), pw]
        depth = rng.uniform(0, 1)
        exp, uv = object_ps_model(c, cur, prev, depth)
        tex = FixedTex(rng, {key('tf9', *uv): [depth, 0, 0, 0]})
        cc = {i: [0.0] * 4 for i in range(256)}
        cc.update(c)
        cc.update(xsim.lits(OBJECT_PS))
        st = xsim.State(cc, {0: cur, 1: prev}, tex.fetch_cb)
        xsim.run(xsim.load(OBJECT_PS), st)
        got = st.o.get('oC0', [math.nan] * 4)
        me = max(me, max(abs(g - e) for g, e in zip(got, exp)))
        if [l[0] for l in tex.log] != ['tf9'] or not same_taps([l[1:] for l in tex.log], [uv]):
            bad += 1
    ok = me < 1e-4 and bad == 0
    print('%s %-34s trials=%d max_err=%.3g tap_mismatch=%d %s'
          % (OBJECT_PS, 'object PS', trials, me, bad, 'PASS' if ok else 'FAIL'))
    return ok


def main(trials=100):
    ok = check_velocity(trials)
    ok &= check_xfm(trials)
    ok &= check_object_vs(OBJECT_VS, True, trials)
    ok &= check_object_vs(OBJECT_VS_RIGID, False, trials)
    ok &= check_object_ps(trials)
    hashes = sorted(os.path.basename(p)[7:23] for p in glob.glob(os.path.join(xsim.DUMP, 'shader_*.ucode.frag'))
                    if 'c122' in open(p).read())
    for h in hashes:
        try:
            ok &= check_substitution(h, max(20, trials // 5))
        except (NotImplementedError, ValueError) as e:
            print('%s %-34s cannot run: %s' % (h, 'composite', e))
            ok = False
    return ok


def negative():
    """deliberately wrong blur models: each must fail check_xfm"""
    global NORM, STEP, SKIP_ZERO
    held = 0
    for name, setup in [('no 1.0054859 normalisation', lambda: globals().update(NORM=1.0)),
                        ('step 0.023 (the start offset)', lambda: globals().update(STEP=0.023)),
                        ('no k = 0 tap', lambda: globals().update(SKIP_ZERO=True))]:
        saved = (NORM, STEP, SKIP_ZERO)
        setup()
        print('negative control:', name)
        held += not check_xfm(40)
        NORM, STEP, SKIP_ZERO = saved
    print('negative controls failed as they should: %d of 3' % held)
    return held == 3


if __name__ == '__main__':
    if len(sys.argv) > 1 and sys.argv[1] == '--neg':
        sys.exit(0 if negative() else 1)
    sys.exit(0 if main(int(sys.argv[1]) if len(sys.argv) > 1 else 100) else 1)
